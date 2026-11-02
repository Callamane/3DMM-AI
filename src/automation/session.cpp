/***************************************************************************
    Automation bridge: session commands (handshake, state, movie files,
    undo/redo).
***************************************************************************/
#include "bridge.h"

#include <cstdlib>

#include "buildgob.h"
#include "sharutil.chh"
#include "version.h"

ASSERTNAME

namespace automation
{

const uint32_t kdtsStudioTimeout = 60 * kdtsSecond;

// Builds an FNI from a user-supplied path. Expands a leading "~/".
bool FFniFromPath(Request &req, const std::string &strPathIn, FTG ftg, FNI *pfni)
{
    std::string strPath = strPathIn;
    STN stn;

    if (strPath.empty())
        return req.Fail(kszErrBadArgs, "path is empty");
    if (strPath.compare(0, 2, "~/") == 0)
    {
        const char *pszHome = getenv("HOME");
        if (pszHome == nullptr)
            pszHome = getenv("USERPROFILE");
        if (pszHome != nullptr)
            strPath = std::string(pszHome) + strPath.substr(1);
    }
    if (strPath.size() > kcchMaxStn)
        return req.Fail(kszErrBadArgs, "path is longer than " + std::to_string(kcchMaxStn) + " characters");

    StnFromStr(strPath, &stn);
    if (!pfni->FBuildFromPath(&stn, ftg))
        return req.Fail(kszErrBadArgs, "invalid path '" + strPathIn + "' (use an absolute path)");
    return fTrue;
}

static void _DiscardChanges(PMVIE pmvie)
{
    // MVIE tracks two dirty flags; clear both so closing doesn't prompt
    pmvie->SetDirty(fFalse);
    pmvie->DOCB::SetDirty(fFalse);
}

/***************************************************************************
    State
***************************************************************************/
Json JsonMovieState(PMVIE pmvie)
{
    Json json = Json::MakeObject();
    STN stn;
    FNI fni;

    pmvie->GetName(&stn);
    json.Set("title", StrFromStn(&stn));
    if (pmvie->FGetFni(&fni))
    {
        fni.GetStnPath(&stn);
        json.Set("path", StrFromStn(&stn));
    }
    else
    {
        json.Set("path", Json());
    }
    json.Set("dirty", pmvie->FDirty());
    json.Set("playing", pmvie->FPlaying());
    json.Set("scene_count", pmvie->Cscen());
    json.Set("scene_index", pmvie->Iscen() == ivNil ? Json() : Json(pmvie->Iscen()));

    PSCEN pscen = pmvie->Pscen();
    if (pscen != pvNil)
    {
        Json jsonScene = Json::MakeObject();
        jsonScene.Set("frame", pscen->Nfrm());
        jsonScene.Set("first_frame", pscen->NfrmFirst());
        jsonScene.Set("last_frame", pscen->NfrmLast());
        jsonScene.Set("frames_per_second", kfps);

        PBKGD pbkgd = pscen->Pbkgd();
        TAG tag;
        if (pbkgd != pvNil)
        {
            pbkgd->GetName(&stn);
            Json jsonBkgd = Json::MakeObject();
            if (pscen->FGetTagBkgd(&tag))
                jsonBkgd.Set("id", "bkgd:" + std::to_string(tag.sid) + ":" + std::to_string(static_cast<uint32_t>(tag.cno)));
            jsonBkgd.Set("name", StrFromStn(&stn));
            jsonScene.Set("background", jsonBkgd);
            jsonScene.Set("camera", pbkgd->Icam());
            jsonScene.Set("camera_count", pbkgd->Ccam());
        }

        PACTR pactr = pscen->PactrSelected();
        if (pactr != pvNil)
        {
            Json jsonActr = Json::MakeObject();
            jsonActr.Set("id", pactr->Arid());
            if (pmvie->FGetName(pactr->Arid(), &stn))
                jsonActr.Set("name", StrFromStn(&stn));
            jsonScene.Set("selected_actor", jsonActr);
        }
        else
        {
            jsonScene.Set("selected_actor", Json());
        }
        jsonScene.Set("actor_count", pscen->Cactr());
        jsonScene.Set("text_box_count", pscen->Ctbox());
        json.Set("scene", jsonScene);
    }
    return json;
}

static bool _FCmdHello(Request &req)
{
    req.result.Set("protocol", kProtocolVersion);
    req.result.Set("app", "3DMMEx");
#ifdef GIT_VERSION
    req.result.Set("version", GIT_VERSION);
#endif
    req.result.Set("studio_open", PmvieCur() != pvNil);
    req.result.Set("frames_per_second", kfps);
    req.result.Set("units", "stage coordinates are metres: x = right, z = away from the camera, y = up; "
                            "origin is the background's default actor placement point");
    return fTrue;
}

static bool _FCmdGetState(Request &req)
{
    PMVIE pmvie = PmvieCur();

    req.result.Set("screen", pmvie != pvNil ? "studio" : "building");
    req.result.Set("modal_dialog_open", vpappb->CactModal() > 0);
    if (pmvie != pvNil)
        req.result.Set("movie", JsonMovieState(pmvie));
    return fTrue;
}

static bool _FPollStudioOpen(Request &req)
{
    if (PmvieCur() != pvNil)
    {
        req.result.Set("screen", "studio");
        return fTrue;
    }
    if (TsCurrent() - req.tsStart > kdtsStudioTimeout)
    {
        req.Fail(kszErrBadState, "timed out waiting for the Studio to open");
        return fTrue;
    }
    return fFalse;
}

static bool _FCmdGotoStudio(Request &req)
{
    if (PmvieCur() != pvNil)
    {
        req.result.Set("screen", "studio");
        return fTrue;
    }

    // Same as clicking into the Studio from the building: the Util gob's
    // "Load Studio" script (util.cht, kchidScript8) tears down the current
    // place and enqueues cidLoadStudio.
    PGOK pgokUtil = (PGOK)vapp.Pkwa()->PgobFromHid(kgobUtil);
    if (pgokUtil == pvNil || !pgokUtil->FIs(kclsGOK))
        return req.Fail(kszErrBadState, "cannot reach the Studio from the current screen");

    int32_t rglw[3] = {kgobStudio1, kstDefault, (int32_t)chidNil};
    if (!pgokUtil->FRunScript(kchidScript8, rglw, 3))
        return FFailEngine(req, "the Studio could not be loaded");

    req.poll = _FPollStudioOpen;
    return fTrue;
}

static bool _FCheckCanClose(Request &req, PMVIE pmvie)
{
    bool fDiscard = fFalse;
    if (!req.FArgBool("discard_changes", &fDiscard, fFalse))
        return fFalse;
    if (pmvie->FPlaying())
        pmvie->Play(); // toggles playback off
    if (pmvie->FDirty() && pmvie->Cscen() > 0)
    {
        if (!fDiscard)
        {
            return req.Fail(kszErrBadState, "the current movie has unsaved changes; call save_movie first or pass "
                                            "\"discard_changes\": true");
        }
        _DiscardChanges(pmvie);
    }
    return fTrue;
}

static bool _FCmdNewMovie(Request &req)
{
    PMVIE pmvie;
    if (!FRequireMovie(req, &pmvie) || !_FCheckCanClose(req, pmvie))
        return fFalse;
    if (!PstdioCur()->FLoadMovie(pvNil))
        return FFailEngine(req, "could not create a new movie");
    req.result = JsonMovieState(PmvieCur());
    return fTrue;
}

static bool _FCmdOpenMovie(Request &req)
{
    PMVIE pmvie;
    std::string strPath;
    FNI fni;

    if (!req.FArgString("path", &strPath) || !FFniFromPath(req, strPath, kftg3mm, &fni))
        return fFalse;
    if (fni.TExists() != tYes)
        return req.Fail(kszErrIo, "no such file: " + strPath);
    if (!FRequireMovie(req, &pmvie) || !_FCheckCanClose(req, pmvie))
        return fFalse;
    if (!PstdioCur()->FLoadMovie(&fni))
        return FFailEngine(req, "could not open '" + strPath + "'");
    req.result = JsonMovieState(PmvieCur());
    return fTrue;
}

static bool _FCmdSaveMovie(Request &req)
{
    PMVIE pmvie;
    std::string strPath;
    FNI fni;
    STN stn;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (pmvie->Cscen() < 1)
        return req.Fail(kszErrBadState, "an empty movie cannot be saved; add a scene first");
    if (!req.FArgString("path", &strPath, fFalse))
        return fFalse;

    if (strPath.empty())
    {
        if (!pmvie->FGetFni(&fni))
            return req.Fail(kszErrBadArgs, "this movie has never been saved; pass 'path' (an absolute .3mm path)");
    }
    else
    {
        if (StrLower(strPath).size() < 4 || StrLower(strPath).compare(StrLower(strPath).size() - 4, 4, ".3mm") != 0)
            strPath += ".3mm";
        if (!FFniFromPath(req, strPath, kftg3mm, &fni))
            return fFalse;
        bool fOverwrite = fTrue;
        if (!req.FArgBool("overwrite", &fOverwrite, fFalse))
            return fFalse;
        FNI fniCur;
        bool fSameFile = pmvie->FGetFni(&fniCur) && fniCur.FEqual(&fni);
        if (!fOverwrite && !fSameFile && fni.TExists() == tYes)
            return req.Fail(kszErrIo, "file exists: " + strPath + " (pass \"overwrite\": true to replace it)");
    }

    if (!pmvie->FSaveToFni(&fni, fTrue))
        return FFailEngine(req, "save failed");
    pmvie->Pmcc()->UpdateTitle(pmvie->PstnTitle());

    fni.GetStnPath(&stn);
    req.result.Set("path", StrFromStn(&stn));
    req.result.Set("scene_count", pmvie->Cscen());
    req.result.Set("note", "saving clears the undo history (same as the Studio's Save)");
    return fTrue;
}

static bool _FCmdUndoRedo(Request &req, bool fRedo)
{
    PMVIE pmvie;
    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;

    int32_t cstep = 1;
    if (!req.FArgInt("steps", &cstep, fFalse))
        return fFalse;
    if (cstep < 1)
        return req.Fail(kszErrBadArgs, "'steps' must be at least 1");

    int32_t istep;
    for (istep = 0; istep < cstep; istep++)
    {
        if (!(fRedo ? pmvie->FRedo() : pmvie->FUndo()))
            break;
    }
    StrDrainEngineErrors();
    RefreshViews(pmvie);
    if (istep == 0)
        return req.Fail(kszErrBadState, fRedo ? "nothing to redo" : "nothing to undo");
    req.result.Set(fRedo ? "redone" : "undone", istep);
    req.result.Set("movie", JsonMovieState(pmvie));
    return fTrue;
}

static bool _FCmdUndo(Request &req)
{
    return _FCmdUndoRedo(req, fFalse);
}

static bool _FCmdRedo(Request &req)
{
    return _FCmdUndoRedo(req, fTrue);
}

static bool _FCmdQuit(Request &req)
{
    PMVIE pmvie = PmvieCur();
    if (pmvie != pvNil && !_FCheckCanClose(req, pmvie))
        return fFalse;
    if (pmvie != pvNil)
        _DiscardChanges(pmvie);
    vapp.Quit(fTrue);
    req.result.Set("quitting", true);
    return fTrue;
}

const CommandInfo krgcmdSession[] = {
    {"hello", _FCmdHello, fFalse},
    {"get_state", _FCmdGetState, fFalse},
    {"goto_studio", _FCmdGotoStudio, fFalse},
    {"new_movie", _FCmdNewMovie, fTrue},
    {"open_movie", _FCmdOpenMovie, fTrue},
    {"save_movie", _FCmdSaveMovie, fTrue},
    {"undo", _FCmdUndo, fTrue},
    {"redo", _FCmdRedo, fTrue},
    {"quit", _FCmdQuit, fFalse},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
