/***************************************************************************
    Automation bridge: observing the movie (scene description, frame
    capture) and playback.
***************************************************************************/
#include "bridge.h"

#include <cmath>

#include "pngwrite.h"

ASSERTNAME

namespace automation
{

Json JsonMovieState(PMVIE pmvie);
Json JsonActor(PMVIE pmvie, PACTR pactr, const StageFrame &stage, const CameraModel &camera);
bool FFniFromPath(Request &req, const std::string &strPathIn, FTG ftg, FNI *pfni);

const uint32_t kdtsPlayTimeoutMax = 15 * 60 * kdtsSecond;

/***************************************************************************
    Frame capture

    Draws the movie view and its child GOBs (text boxes) into an 8-bit
    offscreen port, exactly as they appear on screen, then writes an indexed
    PNG. Works in release builds. Selection highlights are hidden while
    drawing.
***************************************************************************/
bool FRenderToPng(Request &req, const std::string &strPath, int32_t *pdxp, int32_t *pdyp)
{
    PMVIE pmvie;
    PSCEN pscen;
    RC rc;

    if (!FRequireScene(req, &pmvie, &pscen))
        return fFalse;
    PMVU pmvu = PmvuCur();
    if (pmvu == pvNil)
        return req.Fail(kszErrBadState, "no movie view");
    pmvu->GetRc(&rc, cooLocal);
    if (rc.FEmpty())
        return req.Fail(kszErrBadState, "the movie view has no size (is the window minimized?)");

    PACTR pactrSel = pscen->PactrSelected();
    PTBOX ptboxSel = pscen->PtboxSelected();
    if (pactrSel != pvNil)
        pscen->SelectActr(pvNil);
    if (ptboxSel != pvNil)
        pscen->SelectTbox(pvNil);
    // SelectActr doesn't unhilite in text mode, so clear every actor's
    // selection highlight explicitly
    PGL pglpactr = pscen->PglRollCall();
    for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
    {
        PACTR pactr;
        pglpactr->Get(ipactr, &pactr);
        pactr->Unhilite();
    }
    pmvie->Pbwld()->MarkDirty();

    bool fOk = fFalse;
    std::string strError;
    PGPT pgpt = GPT::PgptNewOffscreen(&rc, 8);
    PGL pglclr = GPT::PglclrGetPalette();
    if (pgpt != pvNil && pglclr != pvNil)
    {
        pgpt->SetOffscreenColors(pglclr);
        pmvu->DrawTree(pgpt, pvNil, pvNil, fgobNil);

        RC rcPix;
        uint8_t *prgb = pgpt->PrgbLockPixels(&rcPix);
        // Rows of 8-bit surfaces are padded to 4 bytes
        int32_t cbRow = (pgpt->CbRow() + 3) & ~3;
        std::vector<PngColor> palette;
        for (int32_t iclr = 0; iclr < pglclr->IvMac() && iclr < 256; iclr++)
        {
            CLR clr;
            pglclr->Get(iclr, &clr);
            palette.push_back({clr.bRed, clr.bGreen, clr.bBlue});
        }
        std::vector<uint8_t> png = EncodePngIndexed(prgb, rc.Dxp(), rc.Dyp(), cbRow, palette);
        pgpt->Unlock();
        fOk = FWriteFileBytes(strPath, png, &strError);
        *pdxp = rc.Dxp();
        *pdyp = rc.Dyp();
    }
    else
    {
        strError = "could not create an offscreen buffer";
    }
    ReleasePpo(&pgpt);
    ReleasePpo(&pglclr);

    if (pactrSel != pvNil)
    {
        pscen->SelectActr(pactrSel);
        if (pmvu->FActrMode())
            pactrSel->Hilite();
    }
    if (ptboxSel != pvNil)
        pscen->SelectTbox(ptboxSel);
    pmvie->Pbwld()->MarkDirty();
    pmvie->MarkViews();

    if (!fOk)
        return req.Fail(kszErrIo, strError);
    return fTrue;
}

static std::string _StrDefaultCaptureDir(void)
{
    FNI fni;
    STN stn;
    if (!fni.FGetTemp())
        return ".";
    fni.GetStnPath(&stn);
    std::string str = StrFromStn(&stn);
    while (!str.empty() && (str.back() == '/' || str.back() == '\\'))
        str.pop_back();
    return str;
}

static std::string _StrJoin(const std::string &strDir, const std::string &strLeaf)
{
    if (strDir.empty())
        return strLeaf;
    char chLast = strDir.back();
    if (chLast == '/' || chLast == '\\')
        return strDir + strLeaf;
    return strDir + "/" + strLeaf;
}

static bool _FCmdRenderFrame(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    std::string strPath;
    int32_t dxp = 0, dyp = 0;
    static int32_t _cRender = 0;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("path", &strPath, fFalse))
        return fFalse;

    int32_t nfrmRestore = pscen->Nfrm();
    if (!FApplyFrameArg(req, pscen))
        return fFalse;

    if (strPath.empty())
        strPath = _StrJoin(_StrDefaultCaptureDir(), "3dmm-frame-" + std::to_string(++_cRender) + ".png");
    else if (strPath.compare(0, 2, "~/") == 0 && getenv("HOME") != nullptr)
        strPath = std::string(getenv("HOME")) + strPath.substr(1);

    int32_t nfrm = pscen->Nfrm();
    bool fOk = FRenderToPng(req, strPath, &dxp, &dyp);

    bool fRestore = fTrue;
    if (!req.FArgBool("restore_frame", &fRestore, fFalse))
        return fFalse;
    if (fRestore && nfrmRestore != pscen->Nfrm())
        pscen->FGotoFrm(nfrmRestore);
    if (!fOk)
        return fFalse;

    req.result.Set("path", strPath);
    req.result.Set("width", dxp);
    req.result.Set("height", dyp);
    req.result.Set("scene_index", pmvie->Iscen());
    req.result.Set("frame", nfrm);
    return fTrue;
}

static bool _FCmdExportFrames(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    std::string strDir, strPrefix = "frame";
    int32_t iscen, nfrmFrom, nfrmTo, dnfrm = 1;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("dir", &strDir) || !req.FArgString("prefix", &strPrefix, fFalse) ||
        !req.FArgInt("step", &dnfrm, fFalse))
    {
        return fFalse;
    }
    if (dnfrm < 1)
        return req.Fail(kszErrBadArgs, "'step' must be at least 1");
    if (strDir.compare(0, 2, "~/") == 0 && getenv("HOME") != nullptr)
        strDir = std::string(getenv("HOME")) + strDir.substr(1);

    iscen = pmvie->Iscen();
    if (!req.FArgInt("scene", &iscen, fFalse))
        return fFalse;
    if (iscen < 0 || iscen >= pmvie->Cscen())
        return req.Fail(kszErrBadArgs, "scene " + std::to_string(iscen) + " does not exist");

    int32_t iscenRestore = pmvie->Iscen();
    int32_t nfrmRestore = pscen->Nfrm();
    if (iscen != pmvie->Iscen() && !pmvie->FSwitchScen(iscen))
        return FFailEngine(req, "could not switch to scene " + std::to_string(iscen));
    pscen = pmvie->Pscen();

    nfrmFrom = pscen->NfrmFirst();
    nfrmTo = pscen->NfrmLast();
    if (!req.FArgInt("from", &nfrmFrom, fFalse) || !req.FArgInt("to", &nfrmTo, fFalse))
        return fFalse;
    nfrmFrom = LwMax(nfrmFrom, pscen->NfrmFirst());
    nfrmTo = LwMin(nfrmTo, pscen->NfrmLast());
    if (nfrmTo < nfrmFrom)
        return req.Fail(kszErrBadArgs, "empty frame range");
    if ((nfrmTo - nfrmFrom) / dnfrm > 5000)
        return req.Fail(kszErrBadArgs, "too many frames in one call (limit 5000); use 'step' or a smaller range");

    Json jsonFiles = Json::MakeArray();
    int32_t cfile = 0;
    for (int32_t nfrm = nfrmFrom; nfrm <= nfrmTo; nfrm += dnfrm)
    {
        int32_t dxp, dyp;
        char rgch[32];
        if (!pscen->FGotoFrm(nfrm))
        {
            FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
            break;
        }
        snprintf(rgch, sizeof(rgch), "_s%02d_%04d.png", iscen, cfile);
        std::string strPath = _StrJoin(strDir, strPrefix + rgch);
        if (!FRenderToPng(req, strPath, &dxp, &dyp))
            break;
        if (cfile < 3 || nfrm + dnfrm > nfrmTo)
            jsonFiles.Push(strPath);
        cfile++;
    }

    if (iscenRestore != pmvie->Iscen())
        pmvie->FSwitchScen(iscenRestore);
    if (pmvie->Pscen() != pvNil)
        pmvie->Pscen()->FGotoFrm(nfrmRestore);
    RefreshViews(pmvie);
    if (req.Failed())
        return fFalse;

    req.result.Set("count", cfile);
    req.result.Set("scene", iscen);
    req.result.Set("from", nfrmFrom);
    req.result.Set("to", nfrmTo);
    req.result.Set("frames_per_second", kfps / dnfrm);
    req.result.Set("sample_paths", jsonFiles);
    req.result.Set("pattern", _StrJoin(strDir, strPrefix + "_s" + (iscen < 10 ? "0" : "") + std::to_string(iscen) +
                                                   "_%04d.png"));
    return fTrue;
}

/***************************************************************************
    Scene description
***************************************************************************/
static double _Round2(double d)
{
    return std::floor(d * 100 + 0.5) / 100;
}

static Json _JsonVisibleFloor(const StageFrame &stage, const CameraModel &camera)
{
    Json json = Json::MakeObject();
    double dxp = camera.viewportWidth, dyp = camera.viewportHeight;
    Vec3 world;

    // Sample the floor under a few screen points to describe what's in shot
    struct
    {
        const char *psz;
        double xp, yp;
    } rgpt[] = {
        {"bottom_left", 0, dyp * 0.95},   {"bottom_center", dxp / 2, dyp * 0.95}, {"bottom_right", dxp, dyp * 0.95},
        {"middle_left", 0, dyp * 0.6},    {"middle_center", dxp / 2, dyp * 0.6},  {"middle_right", dxp, dyp * 0.6},
    };
    for (auto &pt : rgpt)
    {
        if (camera.FFloorAt(pt.xp, pt.yp, &world))
        {
            Vec3 st = stage.FromWorld(world);
            Json jsonPt = Json::MakeObject();
            jsonPt.Set("x", _Round2(st.x));
            jsonPt.Set("z", _Round2(st.z));
            json.Set(pt.psz, jsonPt);
        }
    }
    json.Set("note", "stage (x, z) of the floor seen at points on screen. Actors stand on the floor, so a feet "
                     "position between the bottom and middle samples keeps them comfortably in shot.");
    return json;
}

static std::string _StrTboxText(PTBOX ptbox)
{
    int32_t cpMac = ptbox->CpMac();
    std::string str;
    achar rgch[129];
    for (int32_t cp = 0; cp < cpMac - 1; cp += 128)
    {
        int32_t ccp = LwMin(128, cpMac - 1 - cp);
        ptbox->FetchRgch(cp, ccp, rgch);
        rgch[ccp] = 0;
        STN stn;
        stn.SetRgch(rgch, ccp);
        str += StrFromStn(&stn);
        if (str.size() > 1000)
            break;
    }
    for (char &ch : str)
    {
        if (ch == '\r')
            ch = '\n';
    }
    return str;
}

static bool _FCmdDescribeScene(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    StageFrame stage;
    CameraModel camera;

    if (!FRequireScene(req, &pmvie, &pscen))
        return fFalse;
    if (!FRequireNotPlaying(req, pmvie) || !FApplyFrameArg(req, pscen))
        return fFalse;
    if (!FGetStageFrame(pmvie, &stage, &camera))
        return FFailEngine(req, "no camera");

    req.result = JsonMovieState(pmvie);

    Json jsonCam = Json::MakeObject();
    jsonCam.Set("index", pscen->Pbkgd()->Icam());
    jsonCam.Set("count", pscen->Pbkgd()->Ccam());
    jsonCam.Set("position", JsonFromPoint(camera.position, stage, camera).Get("stage"));
    jsonCam.Set("field_of_view_degrees", _Round2(camera.fovYDegrees));
    jsonCam.Set("view_width", static_cast<int>(camera.viewportWidth));
    jsonCam.Set("view_height", static_cast<int>(camera.viewportHeight));
    jsonCam.Set("visible_floor", _JsonVisibleFloor(stage, camera));
    Json jsonMat = Json::MakeArray();
    for (const Vec3 &v : {camera.axisX, camera.axisY, camera.axisZ, camera.position})
    {
        Json jsonRow = Json::MakeArray();
        jsonRow.Push(_Round2(v.x));
        jsonRow.Push(_Round2(v.y));
        jsonRow.Push(_Round2(v.z));
        jsonMat.Push(jsonRow);
    }
    // Camera-to-world matrix rows (X axis, Y axis, Z axis, position); the
    // camera looks down -Z. For callers that want to do their own math.
    jsonCam.Set("world_matrix", jsonMat);
    req.result.Set("camera", jsonCam);

    Json jsonActors = Json::MakeArray();
    PGL pglpactr = pscen->PglRollCall();
    for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
    {
        PACTR pactr;
        pglpactr->Get(ipactr, &pactr);
        jsonActors.Push(JsonActor(pmvie, pactr, stage, camera));
    }
    req.result.Set("actors", jsonActors);

    Json jsonTboxes = Json::MakeArray();
    for (int32_t itbox = 0; itbox < pscen->Ctbox(); itbox++)
    {
        PTBOX ptbox = pscen->PtboxFromItbox(itbox);
        if (ptbox == pvNil)
            continue;
        Json json = Json::MakeObject();
        int32_t nfrmFirst, nfrmLast;
        json.Set("index", itbox);
        json.Set("kind", ptbox->FStory() ? "caption" : "scrolling");
        json.Set("text", _StrTboxText(ptbox));
        json.Set("visible_now", ptbox->FIsVisible());
        if (ptbox->FGetLifetime(&nfrmFirst, &nfrmLast))
        {
            json.Set("first_frame", nfrmFirst);
            json.Set("last_frame", nfrmLast);
        }
        jsonTboxes.Push(json);
    }
    req.result.Set("text_boxes", jsonTboxes);

    // Scene ("background") sounds active at this frame
    Json jsonSounds = Json::MakeArray();
    for (int32_t sty = stySfx; sty < styLim; sty++)
    {
        PGL pgltag = pvNil;
        int32_t vlm;
        bool fLoop;
        if (pscen->FQuerySnd(sty, &pgltag, &vlm, &fLoop) && pgltag != pvNil)
        {
            for (int32_t itag = 0; itag < pgltag->IvMac(); itag++)
            {
                TAG tag;
                pgltag->Get(itag, &tag);
                Json json = Json::MakeObject();
                json.Set("type", sty == styMidi ? "music" : (sty == stySpeech ? "speech" : "sfx"));
                json.Set("cno", static_cast<uint32_t>(tag.cno));
                json.Set("imported", tag.sid == ksidUseCrf);
                json.Set("loop", fLoop);
                jsonSounds.Push(json);
            }
        }
        ReleasePpo(&pgltag);
    }
    req.result.Set("scene_sounds_at_frame", jsonSounds);
    return fTrue;
}

/***************************************************************************
    Playback
***************************************************************************/
static bool _FPollPlaying(Request &req)
{
    PMVIE pmvie = PmvieCur();
    if (pmvie == pvNil || !pmvie->FPlaying())
    {
        if (pmvie != pvNil)
            req.result.Set("movie", JsonMovieState(pmvie));
        req.result.Set("finished", true);
        return fTrue;
    }
    double dSecMax = 120;
    if (req.args.Get("timeout_seconds").IsNumber())
        dSecMax = req.args.Get("timeout_seconds").AsNumber();
    if (TsCurrent() - req.tsStart > LwMin(static_cast<int32_t>(dSecMax * kdtsSecond), kdtsPlayTimeoutMax))
    {
        pmvie->Play(); // stop
        req.result.Set("finished", false);
        req.result.Set("note", "stopped after the timeout");
        return fTrue;
    }
    return fFalse;
}

static bool _FCmdPlay(Request &req)
{
    PMVIE pmvie;
    bool fWait = fFalse, fFromStart = fFalse;

    if (!FRequireMovie(req, &pmvie) || !req.FArgBool("wait", &fWait, fFalse) ||
        !req.FArgBool("from_start", &fFromStart, fFalse))
    {
        return fFalse;
    }
    if (pmvie->Cscen() == 0)
        return req.Fail(kszErrBadState, "the movie has no scenes");
    if (pmvie->FPlaying())
        return req.Fail(kszErrBadState, "already playing");

    if (fFromStart)
    {
        if (pmvie->Iscen() != 0 && !pmvie->FSwitchScen(0))
            return FFailEngine(req, "could not go to the first scene");
        pmvie->Pscen()->FGotoFrm(pmvie->Pscen()->NfrmFirst());
    }
    else if (req.FHasArg("scene"))
    {
        int32_t iscen;
        if (!req.FArgInt("scene", &iscen))
            return fFalse;
        if (iscen < 0 || iscen >= pmvie->Cscen())
            return req.Fail(kszErrBadArgs, "scene " + std::to_string(iscen) + " does not exist");
        if (iscen != pmvie->Iscen() && !pmvie->FSwitchScen(iscen))
            return FFailEngine(req, "could not switch scenes");
        if (!FApplyFrameArg(req, pmvie->Pscen()))
            return fFalse;
    }
    else if (!FApplyFrameArg(req, pmvie->Pscen()))
    {
        return fFalse;
    }

    // Like pressing Play in the Studio: plays from the current frame to the
    // end of the movie
    vpcex->EnqueueCid(cidPlay, PstdioCur());
    req.result.Set("started_scene", pmvie->Iscen());
    req.result.Set("started_frame", pmvie->Pscen()->Nfrm());
    if (fWait)
    {
        // Give the queued cidPlay a chance to start before polling
        req.poll = [](Request &req) {
            PMVIE pmvie = PmvieCur();
            if (pmvie != pvNil && !pmvie->FPlaying() && TsCurrent() - req.tsStart < kdtsSecond)
                return false;
            return _FPollPlaying(req);
        };
    }
    return fTrue;
}

static bool _FCmdStop(Request &req)
{
    PMVIE pmvie;
    if (!FRequireMovie(req, &pmvie))
        return fFalse;
    bool fWasPlaying = pmvie->FPlaying();
    if (fWasPlaying)
        vpcex->EnqueueCid(cidPlay, PstdioCur()); // toggles playback off
    req.result.Set("was_playing", fWasPlaying);
    return fTrue;
}

const CommandInfo krgcmdObserve[] = {
    {"describe_scene", _FCmdDescribeScene, fTrue},
    {"render_frame", _FCmdRenderFrame, fTrue},
    {"export_frames", _FCmdExportFrames, fTrue},
    {"play", _FCmdPlay, fTrue},
    {"stop", _FCmdStop, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
