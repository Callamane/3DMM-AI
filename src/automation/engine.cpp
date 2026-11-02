/***************************************************************************
    Automation bridge: helpers shared by the command modules (engine access,
    actor lookup, coordinates, engine error reporting).
***************************************************************************/
#include "bridge.h"

#include <cctype>
#include <cmath>

ASSERTNAME

namespace automation
{

/***************************************************************************
    Strings
***************************************************************************/
std::string StrFromStn(PSTN pstn)
{
    U8SZ u8sz;
    pstn->GetUtf8Sz(u8sz);
    return std::string(u8sz);
}

void StnFromStr(const std::string &str, PSTN pstn)
{
    // STN holds at most kcchMaxStn characters; SetUtf8Sz truncates
    std::string strT = str.substr(0, kcchMaxUtf8Sz);
    pstn->SetUtf8Sz(const_cast<char *>(strT.c_str()));
}

std::string StrLower(const std::string &str)
{
    std::string strLower = str;
    for (char &ch : strLower)
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return strLower;
}

/***************************************************************************
    Current studio / movie / view
***************************************************************************/
PSTDIO PstdioCur(void)
{
    return vapp.Pstdio();
}

PMVIE PmvieCur(void)
{
    PSTDIO pstdio = PstdioCur();
    return pstdio == pvNil ? pvNil : pstdio->Pmvie();
}

PMVU PmvuCur(void)
{
    PMVIE pmvie = PmvieCur();
    if (pmvie == pvNil)
        return pvNil;
    return (PMVU)pmvie->PddgGet(0);
}

bool FRequireMovie(Request &req, PMVIE *ppmvie)
{
    *ppmvie = PmvieCur();
    if (*ppmvie == pvNil)
        return req.Fail(kszErrBadState, "the Studio is not open; call goto_studio first");
    return fTrue;
}

bool FRequireScene(Request &req, PMVIE *ppmvie, PSCEN *ppscen)
{
    if (!FRequireMovie(req, ppmvie))
        return fFalse;
    *ppscen = (*ppmvie)->Pscen();
    if (*ppscen == pvNil)
        return req.Fail(kszErrBadState, "the movie has no scenes yet; call add_scene first");
    return fTrue;
}

bool FRequireNotPlaying(Request &req, PMVIE pmvie)
{
    if (pmvie->FPlaying())
        return req.Fail(kszErrBadState, "the movie is playing; call stop first");
    return fTrue;
}

/***************************************************************************
    Actors
***************************************************************************/
bool FResolveActor(Request &req, const char *pszName, PACTR *ppactr)
{
    PMVIE pmvie;
    PSCEN pscen;
    const Json &json = req.args.Get(pszName);

    *ppactr = pvNil;
    if (!FRequireScene(req, &pmvie, &pscen))
        return fFalse;

    if (json.IsNull())
    {
        *ppactr = pscen->PactrSelected();
        if (*ppactr == pvNil)
            return req.Fail(kszErrBadState, std::string("no actor selected; pass '") + pszName +
                                                "' (an actor id from describe_scene or the actor's name)");
        return fTrue;
    }

    // Numeric id (number or numeric string)
    int32_t arid = aridNil;
    bool fNumeric = fFalse;
    if (json.IsNumber())
    {
        arid = static_cast<int32_t>(json.AsNumber());
        fNumeric = fTrue;
    }
    else if (json.IsString() && !json.AsString().empty())
    {
        char *pchEnd = nullptr;
        long lw = strtol(json.AsString().c_str(), &pchEnd, 10);
        if (pchEnd != nullptr && *pchEnd == '\0')
        {
            arid = static_cast<int32_t>(lw);
            fNumeric = fTrue;
        }
    }
    else
    {
        return req.Fail(kszErrBadArgs, std::string("'") + pszName + "' must be an actor id or name");
    }

    if (fNumeric)
    {
        *ppactr = pscen->PactrFromArid(arid);
        if (*ppactr == pvNil)
        {
            return req.Fail(kszErrNotFound, "no actor with id " + std::to_string(arid) +
                                                 " in the current scene; use describe_scene to list actors");
        }
        return fTrue;
    }

    // By name (case-insensitive), within the current scene
    std::string strWant = StrLower(json.AsString());
    std::string strNames;
    PGL pglpactr = pscen->PglRollCall();
    PACTR pactrMatch = pvNil;
    int32_t cmatch = 0;
    for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
    {
        PACTR pactr;
        STN stn;
        pglpactr->Get(ipactr, &pactr);
        if (!pmvie->FGetName(pactr->Arid(), &stn))
            continue;
        std::string strName = StrFromStn(&stn);
        if (!strNames.empty())
            strNames += ", ";
        strNames += "'" + strName + "' (id " + std::to_string(pactr->Arid()) + ")";
        if (StrLower(strName) == strWant)
        {
            pactrMatch = pactr;
            cmatch++;
        }
    }
    if (cmatch == 1)
    {
        *ppactr = pactrMatch;
        return fTrue;
    }
    if (cmatch > 1)
    {
        return req.Fail(kszErrBadArgs, "more than one actor is named '" + json.AsString() +
                                           "'; use the numeric id instead. Actors: " + strNames);
    }
    return req.Fail(kszErrNotFound, "no actor named '" + json.AsString() + "' in the current scene. " +
                                        (strNames.empty() ? std::string("The scene has no actors.")
                                                          : "Actors: " + strNames));
}

/***************************************************************************
    Camera and stage frame
***************************************************************************/
bool FGetCamera(PMVIE pmvie, CameraModel *pcamera)
{
    BMAT34 bmat34;
    BRS zrHither, zrYon;
    BRA aFov;
    RC rc;

    if (pmvie == pvNil || pmvie->Pscen() == pvNil || pmvie->Pbwld() == pvNil)
        return fFalse;

    pmvie->Pbwld()->GetCamera(&bmat34, &zrHither, &zrYon, &aFov);
    for (int irow = 0; irow < 4; irow++)
    {
        Vec3 v(DFromBrs(bmat34.m[irow][0]), DFromBrs(bmat34.m[irow][1]), DFromBrs(bmat34.m[irow][2]));
        if (irow == 0)
            pcamera->axisX = v;
        else if (irow == 1)
            pcamera->axisY = v;
        else if (irow == 2)
            pcamera->axisZ = v;
        else
            pcamera->position = v;
    }
    pcamera->fovYDegrees = DegFromBra(aFov);
    pcamera->hither = DFromBrs(zrHither);
    pcamera->yon = DFromBrs(zrYon);

    PMVU pmvu = (PMVU)pmvie->PddgGet(0);
    if (pmvu != pvNil)
    {
        pmvu->GetRc(&rc, cooLocal);
        if (!rc.FEmpty())
        {
            pcamera->viewportWidth = rc.Dxp();
            pcamera->viewportHeight = rc.Dyp();
        }
    }
    return fTrue;
}

bool FGetStageFrame(PMVIE pmvie, StageFrame *pstage, CameraModel *pcamera)
{
    CameraModel camera;
    BRS xr, yr, zr;

    if (!FGetCamera(pmvie, &camera))
        return fFalse;

    pmvie->Pscen()->Pbkgd()->GetPrimaryPlacePoint(&xr, &yr, &zr);
    Vec3 place(DFromBrs(xr), DFromBrs(yr), DFromBrs(zr));
    if (xr == rZero && yr == rZero && zr == rZero)
    {
        // No authored placement point: same fallback as ACTR::_GetNewOrigin,
        // 25 units in front of the camera.
        place = camera.position + camera.axisZ * DFromBrs(kzrDefault);
    }

    pstage->Init(camera, place);
    if (pcamera != pvNil)
        *pcamera = camera;
    return fTrue;
}

static bool _FNumberFromJson(const Json &json, double *pd)
{
    if (json.IsNumber())
    {
        *pd = json.AsNumber();
        return fTrue;
    }
    return fFalse;
}

bool FParsePosition(Request &req, const Json &json, const StageFrame &stage, Vec3 *pworld)
{
    if (json.IsArray() && json.Size() >= 2)
    {
        // [x, z] or [x, y, z] shorthand in stage metres
        double rg[3] = {0, 0, 0};
        for (size_t i = 0; i < json.Size() && i < 3; i++)
        {
            if (!_FNumberFromJson(json.AsArray()[i], &rg[i]))
                return req.Fail(kszErrBadArgs, "position arrays must contain numbers");
        }
        if (json.Size() == 2)
            *pworld = stage.ToWorld(Vec3(rg[0], 0, rg[1]));
        else
            *pworld = stage.ToWorld(Vec3(rg[0], rg[1], rg[2]));
        return fTrue;
    }

    if (!json.IsObject())
    {
        return req.Fail(kszErrBadArgs,
                        "a position must be {\"x\": metres right, \"z\": metres away from camera} or {\"world\": [x,y,z]}");
    }

    const Json &jsonWorld = json.Get("world");
    if (!jsonWorld.IsNull())
    {
        if (!jsonWorld.IsArray() || jsonWorld.Size() != 3 || !_FNumberFromJson(jsonWorld.AsArray()[0], &pworld->x) ||
            !_FNumberFromJson(jsonWorld.AsArray()[1], &pworld->y) ||
            !_FNumberFromJson(jsonWorld.AsArray()[2], &pworld->z))
        {
            return req.Fail(kszErrBadArgs, "\"world\" must be an array of three numbers [x, y, z]");
        }
        return fTrue;
    }

    Vec3 pos;
    if (!json.Get("x").IsNull() && !_FNumberFromJson(json.Get("x"), &pos.x))
        return req.Fail(kszErrBadArgs, "position \"x\" must be a number");
    if (!json.Get("y").IsNull() && !_FNumberFromJson(json.Get("y"), &pos.y))
        return req.Fail(kszErrBadArgs, "position \"y\" must be a number");
    if (!json.Get("z").IsNull() && !_FNumberFromJson(json.Get("z"), &pos.z))
        return req.Fail(kszErrBadArgs, "position \"z\" must be a number");
    *pworld = stage.ToWorld(pos);
    return fTrue;
}

static double _Round(double d, double rQuantum)
{
    return std::floor(d / rQuantum + 0.5) * rQuantum;
}

Json JsonFromPoint(const Vec3 &world, const StageFrame &stage, const CameraModel &camera)
{
    Json json = Json::MakeObject();
    Vec3 st = stage.FromWorld(world);
    double xp, yp;
    bool fInView;

    Json jsonStage = Json::MakeObject();
    jsonStage.Set("x", _Round(st.x, 0.01));
    jsonStage.Set("y", _Round(st.y, 0.01));
    jsonStage.Set("z", _Round(st.z, 0.01));
    json.Set("stage", jsonStage);

    Json jsonWorld = Json::MakeArray();
    jsonWorld.Push(_Round(world.x, 0.01));
    jsonWorld.Push(_Round(world.y, 0.01));
    jsonWorld.Push(_Round(world.z, 0.01));
    json.Set("world", jsonWorld);

    if (camera.FProject(world, &xp, &yp, &fInView))
    {
        Json jsonScreen = Json::MakeObject();
        jsonScreen.Set("x", static_cast<int>(xp));
        jsonScreen.Set("y", static_cast<int>(yp));
        json.Set("screen", jsonScreen);
    }
    json.Set("in_view", fInView);
    return json;
}

/***************************************************************************
    Frames
***************************************************************************/
bool FGotoFrame(Request &req, PSCEN pscen, int32_t nfrm)
{
    if (nfrm < pscen->NfrmFirst())
    {
        return req.Fail(kszErrBadArgs, "frame " + std::to_string(nfrm) + " is before the start of the scene (first frame is " +
                                           std::to_string(pscen->NfrmFirst()) + ")");
    }
    if (nfrm == pscen->Nfrm())
        return fTrue;
    if (!pscen->FGotoFrm(nfrm))
        return FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
    pscen->Pmvie()->Pmcc()->UpdateScrollbars();
    return fTrue;
}

bool FApplyFrameArg(Request &req, PSCEN pscen)
{
    int32_t nfrm;
    if (!req.FHasArg("frame"))
        return fTrue;
    if (!req.FArgInt("frame", &nfrm))
        return fFalse;
    return FGotoFrame(req, pscen, nfrm);
}

/***************************************************************************
    Engine errors
***************************************************************************/
struct ErcName
{
    int32_t erc;
    const char *psz;
};

static const ErcName _rgercname[] = {
    {ercOomHq, "out of memory"},
    {ercOomPv, "out of memory"},
    {ercOomNew, "out of memory"},
    {ercFileGeneral, "file error"},
    {ercFileOpen, "cannot open file"},
    {ercFileCreate, "cannot create file"},
    {ercSocSaveFailure, "save failed"},
    {ercSocSceneSwitch, "could not switch scenes"},
    {ercSocBadFile, "not a valid movie file"},
    {ercSocNoTboxSelected, "no text box selected"},
    {ercSocNoActrSelected, "no actor selected"},
    {ercSocNotUndoable, "the edit worked but cannot be undone"},
    {ercSocNoScene, "no scene"},
    {ercSocBadVersion, "movie file version not supported"},
    {ercSocBadFrameSlider, "frame position error"},
    {ercSocGotoFrameFailure, "could not go to frame"},
    {ercSocActionNotApplicable, "action does not apply to this actor"},
    {ercSocNameTooLong, "name too long"},
    {ercSocTboxTooSmall, "text box too small"},
    {ercSocTdtTooLong, "3D text too long"},
    {ercSocBadTdf, "bad 3D font"},
    {ercSocNoActrMidi, "actors cannot have music (MIDI) sounds; attach music to the scene"},
    {ercSoc3DWordCreate, "could not create 3D word"},
    {ercSocBadSceneSound, "bad scene sound"},
    {ercSocBadSoundFile, "unsupported or corrupt sound file"},
    {ercSocNoDefaultFont, "no default font"},
    {ercSocCantCacheTag, "content could not be loaded"},
    {ercSocInvalidFilename, "invalid file name"},
    {ercSocCantCopyMsnd, "could not copy sound"},
};

std::string StrDrainEngineErrors(void)
{
    std::string str;
    int32_t erc;

    while (vpers->FPop(&erc))
    {
        const char *psz = nullptr;
        for (const ErcName &ercname : _rgercname)
        {
            if (ercname.erc == erc)
            {
                psz = ercname.psz;
                break;
            }
        }
        if (!str.empty())
            str += "; ";
        if (psz != nullptr)
            str += psz;
        str += " (erc " + std::to_string(erc) + ")";
    }
    return str;
}

bool FFailEngine(Request &req, const std::string &strWhat)
{
    std::string strErrors = StrDrainEngineErrors();
    if (strErrors.empty())
        return req.Fail(kszErrEngine, strWhat);
    return req.Fail(kszErrEngine, strWhat + ": " + strErrors);
}

void RefreshViews(PMVIE pmvie)
{
    if (pmvie == pvNil)
        return;
    pmvie->Pbwld()->MarkDirty();
    pmvie->InvalViewsAndScb();
}

} // namespace automation
