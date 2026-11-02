/***************************************************************************
    Automation bridge: observing the movie (scene description, frame
    capture) and playback.
***************************************************************************/
#include "bridge.h"

#include <cmath>
#include <cstring>
#include <filesystem>

#include "image8.h"
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
    offscreen port, exactly as they appear on screen. Works in release
    builds. Selection highlights are hidden while drawing.
***************************************************************************/
static std::vector<PngColor> _PaletteCur(PBKGD pbkgdOverride = pvNil)
{
    std::vector<PngColor> palette;
    PGL pglclr = GPT::PglclrGetPalette();
    for (int32_t iclr = 0; pglclr != pvNil && iclr < pglclr->IvMac() && iclr < 256; iclr++)
    {
        CLR clr;
        pglclr->Get(iclr, &clr);
        palette.push_back({clr.bRed, clr.bGreen, clr.bBlue});
    }
    ReleasePpo(&pglclr);

    // Another background's pictures use that background's palette entries
    PGL pglclrBkgd = pvNil;
    int32_t iclrMin;
    if (pbkgdOverride != pvNil && pbkgdOverride->FGetPalette(&pglclrBkgd, &iclrMin) && pglclrBkgd != pvNil)
    {
        for (int32_t iclr = 0; iclr < pglclrBkgd->IvMac() && iclrMin + iclr < (int32_t)palette.size(); iclr++)
        {
            CLR clr;
            pglclrBkgd->Get(iclr, &clr);
            palette[iclrMin + iclr] = {clr.bRed, clr.bGreen, clr.bBlue};
        }
    }
    ReleasePpo(&pglclrBkgd);
    return palette;
}

static void _CopyPixels(PGPT pgpt, RC *prc, Image8 *pimg)
{
    RC rcPix;
    uint8_t *prgb = pgpt->PrgbLockPixels(&rcPix);
    // Rows of 8-bit surfaces are padded to 4 bytes
    int32_t cbRow = (pgpt->CbRow() + 3) & ~3;
    *pimg = Image8(prc->Dxp(), prc->Dyp());
    for (int32_t y = 0; y < prc->Dyp(); y++)
        memcpy(&pimg->pixels[static_cast<size_t>(y) * prc->Dxp()], prgb + static_cast<size_t>(y) * cbRow, prc->Dxp());
    pgpt->Unlock();
}

// Captures the movie view (3D world plus text boxes) at the current frame.
// fWorldOnly skips text boxes (used for camera previews).
static bool _FCaptureView(Request &req, Image8 *pimg, bool fWorldOnly = fFalse)
{
    PMVIE pmvie = PmvieCur();
    PMVU pmvu = PmvuCur();
    RC rc;

    if (pmvie == pvNil || pmvu == pvNil)
        return req.Fail(kszErrBadState, "no movie view");
    pmvu->GetRc(&rc, cooLocal);
    if (rc.FEmpty())
        return req.Fail(kszErrBadState, "the movie view has no size (is the window minimized?)");

    PSCEN pscen = pmvie->Pscen();
    PACTR pactrSel = pscen != pvNil ? pscen->PactrSelected() : pvNil;
    PTBOX ptboxSel = pscen != pvNil ? pscen->PtboxSelected() : pvNil;
    if (pactrSel != pvNil)
        pscen->SelectActr(pvNil);
    if (ptboxSel != pvNil)
        pscen->SelectTbox(pvNil);
    // SelectActr doesn't unhilite in text mode, so clear every actor's
    // selection highlight explicitly
    PGL pglpactr = pscen != pvNil ? pscen->PglRollCall() : pvNil;
    for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
    {
        PACTR pactr;
        pglpactr->Get(ipactr, &pactr);
        pactr->Unhilite();
    }
    pmvie->Pbwld()->MarkDirty();

    bool fOk = fFalse;
    PGPT pgpt = GPT::PgptNewOffscreen(&rc, 8);
    PGL pglclr = GPT::PglclrGetPalette();
    if (pgpt != pvNil && pglclr != pvNil)
    {
        pgpt->SetOffscreenColors(pglclr);
        if (fWorldOnly)
        {
            GNV gnv(pgpt);
            pmvie->Pbwld()->Render();
            pmvie->Pbwld()->Draw(&gnv, &rc, 0, 0);
        }
        else
        {
            pmvu->DrawTree(pgpt, pvNil, pvNil, fgobNil);
        }
        _CopyPixels(pgpt, &rc, pimg);
        fOk = fTrue;
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
        return req.Fail(kszErrIo, "could not create an offscreen buffer");
    return fTrue;
}

// Draws the stage grid and a label over every on-screen actor.
static void _Annotate(Image8 *pimg, PMVIE pmvie, const std::vector<PngColor> &palette)
{
    StageFrame stage;
    CameraModel camera;
    OverlayColors colors;

    if (!FGetStageFrame(pmvie, &stage, &camera))
        return;
    colors.bGrid = BNearestColor(palette, 0, 200, 220);
    colors.bAxis = BNearestColor(palette, 255, 255, 255);
    colors.bText = BNearestColor(palette, 255, 240, 0);
    colors.bOutline = BNearestColor(palette, 0, 0, 0);
    DrawStageGrid(pimg, camera, stage, -4, 4, -2, 6, colors);

    // Actor labels: "#id NAME" above each actor's on-screen bounds
    EnsureRendered(pmvie);
    PGL pglpactr = pmvie->Pscen()->PglRollCall();
    for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
    {
        PACTR pactr;
        STN stn;
        RC rc;
        pglpactr->Get(ipactr, &pactr);
        if (!pactr->FOnStage() || !pactr->FIsInView())
            continue;
        pactr->GetRcBounds(&rc);
        std::string strLabel = "#" + std::to_string(pactr->Arid());
        if (pmvie->FGetName(pactr->Arid(), &stn))
            strLabel += " " + StrFromStn(&stn).substr(0, 14);
        int32_t dx = DxText(strLabel, 1);
        int32_t x = LwBound((rc.xpLeft + rc.xpRight - dx) / 2, 1, LwMax(1, pimg->width - dx - 1));
        int32_t y = LwBound(rc.ypTop - 9, 1, pimg->height - 7);
        DrawText(pimg, x, y, strLabel, colors.bAxis, colors.bOutline, 1);
    }
}

static bool _FWriteImage(Request &req, const std::string &strPath, const Image8 &img,
                         const std::vector<PngColor> &palette)
{
    std::string strError;
    std::vector<uint8_t> png = EncodePngIndexed(img.pixels.data(), img.width, img.height, img.width, palette);
    if (!FWriteFileBytes(strPath, png, &strError))
        return req.Fail(kszErrIo, strError);
    return fTrue;
}

bool FRenderToPng(Request &req, const std::string &strPath, int32_t *pdxp, int32_t *pdyp, bool fOverlay)
{
    PMVIE pmvie;
    PSCEN pscen;
    Image8 img;

    if (!FRequireScene(req, &pmvie, &pscen) || !_FCaptureView(req, &img))
        return fFalse;
    std::vector<PngColor> palette = _PaletteCur();
    if (fOverlay)
        _Annotate(&img, pmvie, palette);
    *pdxp = img.width;
    *pdyp = img.height;
    return _FWriteImage(req, strPath, img, palette);
}

static std::string _StrDefaultCaptureDir(void)
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::temp_directory_path(ec);
    if (ec)
        return ".";
    return path.string();
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

    bool fOverlay = fFalse;
    if (!req.FArgBool("overlay", &fOverlay, fFalse))
        return fFalse;
    int32_t nfrm = pscen->Nfrm();
    bool fOk = FRenderToPng(req, strPath, &dxp, &dyp, fOverlay);

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

/***************************************************************************
    Camera previews: every camera of a background on one contact sheet
***************************************************************************/
const int32_t kcColumnsPreview = 4;
const int32_t kdzpGapPreview = 4;

static bool _FCmdPreviewCameras(Request &req)
{
    PMVIE pmvie;
    std::string strBkgd, strPath;
    static int32_t _cPreview = 0;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("background", &strBkgd, fFalse) || !req.FArgString("path", &strPath, fFalse))
        return fFalse;

    PSCEN pscen = pmvie->Pscen();
    PBKGD pbkgd = pvNil;
    bool fOther = fFalse;
    if (strBkgd.empty())
    {
        if (pscen == pvNil)
            return req.Fail(kszErrBadArgs, "pass 'background' (there is no scene yet to preview)");
        pbkgd = pscen->Pbkgd();
        pbkgd->AddRef();
    }
    else
    {
        const CatalogEntry *pentry;
        TAG tagScene;
        if (!FFindContent(req, strBkgd, {ContentKind::Background}, &pentry))
            return fFalse;
        TAG tag = pentry->tag;
        if (pscen != pvNil && pscen->FGetTagBkgd(&tagScene) && tagScene.cno == tag.cno && tagScene.sid == tag.sid)
        {
            pbkgd = pscen->Pbkgd();
            pbkgd->AddRef();
        }
        else
        {
            if (!BKGD::FCacheToHD(&tag))
                return FFailEngine(req, "could not load background '" + pentry->name + "'");
            pbkgd = (PBKGD)vptagm->PbacoFetch(&tag, BKGD::FReadBkgd);
            if (pbkgd == pvNil)
                return FFailEngine(req, "could not load background '" + pentry->name + "'");
            fOther = fTrue;
        }
    }

    std::vector<int32_t> vecCams;
    const Json &jsonCams = req.args.Get("cameras");
    if (jsonCams.IsArray())
    {
        for (const Json &json : jsonCams.AsArray())
        {
            int32_t icam = json.IsNumber() ? static_cast<int32_t>(json.AsNumber()) : -1;
            if (icam < 0 || icam >= pbkgd->Ccam())
            {
                ReleasePpo(&pbkgd);
                return req.Fail(kszErrBadArgs, "camera indexes must be 0-" + std::to_string(pbkgd->Ccam() - 1));
            }
            vecCams.push_back(icam);
        }
    }
    else
    {
        for (int32_t icam = 0; icam < pbkgd->Ccam(); icam++)
            vecCams.push_back(icam);
    }

    // Other backgrounds are previewed empty; the current one shows its actors
    int32_t icamRestore = pscen != pvNil ? pscen->Pbkgd()->Icam() : ivNil;
    if (fOther && pscen != pvNil)
        pscen->HideActors();

    std::vector<PngColor> palette = _PaletteCur(fOther ? pbkgd : pvNil);
    uint8_t bText = BNearestColor(palette, 255, 240, 0);
    uint8_t bOutline = BNearestColor(palette, 0, 0, 0);
    Image8 imgSheet;
    Json jsonCells = Json::MakeArray();
    bool fOk = fTrue;
    for (size_t i = 0; i < vecCams.size(); i++)
    {
        Image8 img;
        if (!pbkgd->FSetCamera(pmvie->Pbwld(), vecCams[i]) || !_FCaptureView(req, &img, fTrue))
        {
            if (!req.Failed())
                FFailEngine(req, "could not render camera " + std::to_string(vecCams[i]));
            fOk = fFalse;
            break;
        }
        Image8 imgSmall = Downscale2x(img);
        if (imgSheet.width == 0)
        {
            int32_t crow = (static_cast<int32_t>(vecCams.size()) + kcColumnsPreview - 1) / kcColumnsPreview;
            int32_t ccol = LwMin(kcColumnsPreview, static_cast<int32_t>(vecCams.size()));
            imgSheet = Image8(ccol * imgSmall.width + (ccol - 1) * kdzpGapPreview,
                              crow * imgSmall.height + (crow - 1) * kdzpGapPreview, bOutline);
        }
        int32_t col = static_cast<int32_t>(i) % kcColumnsPreview, row = static_cast<int32_t>(i) / kcColumnsPreview;
        int32_t x = col * (imgSmall.width + kdzpGapPreview), y = row * (imgSmall.height + kdzpGapPreview);
        DrawText(&imgSmall, 4, 4, "CAM " + std::to_string(vecCams[i]), bText, bOutline, 2);
        Blit(&imgSheet, imgSmall, x, y);
        Json jsonCell = Json::MakeObject();
        jsonCell.Set("camera", vecCams[i]);
        jsonCell.Set("row", row);
        jsonCell.Set("column", col);
        jsonCells.Push(jsonCell);
    }

    // Put the world back the way the scene had it
    if (fOther)
        pbkgd->TurnOffLights();
    if (pscen != pvNil)
    {
        pscen->Pbkgd()->FSetCamera(pmvie->Pbwld(), icamRestore);
        if (fOther)
            pscen->ShowActors();
    }
    pmvie->Pbwld()->MarkDirty();
    pmvie->InvalViews();
    ReleasePpo(&pbkgd);
    if (!fOk)
        return fFalse;

    if (strPath.empty())
        strPath = _StrJoin(_StrDefaultCaptureDir(), "3dmm-cameras-" + std::to_string(++_cPreview) + ".png");
    if (!_FWriteImage(req, strPath, imgSheet, palette))
        return fFalse;
    req.result.Set("path", strPath);
    req.result.Set("width", imgSheet.width);
    req.result.Set("height", imgSheet.height);
    req.result.Set("columns", kcColumnsPreview);
    req.result.Set("cells", jsonCells);
    req.result.Set("note", "each cell is labelled CAM n; pass that number to add_scene or set_camera");
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
        if (!FRenderToPng(req, strPath, &dxp, &dyp, fFalse))
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
    req.result.Set("pattern",
                   _StrJoin(strDir, strPrefix + "_s" + (iscen < 10 ? "0" : "") + std::to_string(iscen) + "_%04d.png"));
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
        {"bottom_left", 0, dyp * 0.95}, {"bottom_center", dxp / 2, dyp * 0.95}, {"bottom_right", dxp, dyp * 0.95},
        {"middle_left", 0, dyp * 0.6},  {"middle_center", dxp / 2, dyp * 0.6},  {"middle_right", dxp, dyp * 0.6},
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
    bool fVerbose = fFalse;
    if (!req.FArgBool("verbose", &fVerbose, fFalse))
        return fFalse;
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
    if (fVerbose)
        jsonCam.Set("world_matrix", jsonMat);
    req.result.Set("camera", jsonCam);

    EnsureRendered(pmvie);
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
    Timeline: what happens when, by stepping through the scene's frames
***************************************************************************/
struct ActorSample
{
    bool fOnStage;
    int32_t anid;
    bool fFrozen;
    Vec3 stage;
};

static Json _JsonXZ(const Vec3 &v)
{
    Json json = Json::MakeObject();
    json.Set("x", _Round2(v.x));
    json.Set("z", _Round2(v.z));
    return json;
}

static bool _FCmdDescribeTimeline(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    StageFrame stage;
    CameraModel camera;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    int32_t iscenRestore = pmvie->Iscen();
    if (req.FHasArg("scene"))
    {
        int32_t iscen;
        if (!req.FArgInt("scene", &iscen))
            return fFalse;
        if (iscen < 0 || iscen >= pmvie->Cscen())
            return req.Fail(kszErrBadArgs, "scene " + std::to_string(iscen) + " does not exist");
        if (iscen != pmvie->Iscen() && !pmvie->FSwitchScen(iscen))
            return FFailEngine(req, "could not switch to scene " + std::to_string(iscen));
        pscen = pmvie->Pscen();
    }
    int32_t nfrmRestore = pscen->Nfrm();
    int32_t nfrmFirst = pscen->NfrmFirst(), nfrmLast = pscen->NfrmLast();
    // Report positions in the stage coordinates of the scene's opening
    // camera, where actors are usually placed
    if (!pscen->FGotoFrm(nfrmFirst))
        return FFailEngine(req, "could not go to the first frame");
    int32_t icamCoords = pscen->Pbkgd()->Icam();
    if (!FGetStageFrame(pmvie, &stage, &camera))
        return FFailEngine(req, "no camera");

    PGL pglpactr = pscen->PglRollCall();
    int32_t cactr = pglpactr == pvNil ? 0 : pglpactr->IvMac();
    std::vector<std::vector<ActorSample>> vecvecSamples(cactr);
    std::vector<std::vector<int32_t>> vecvecSoundFrames(cactr);
    std::vector<int32_t> vecCams;
    std::vector<std::vector<bool>> vecvecTboxVisible(pscen->Ctbox());
    Json jsonSceneSounds = Json::MakeArray();

    for (int32_t nfrm = nfrmFirst; nfrm <= nfrmLast; nfrm++)
    {
        if (!pscen->FGotoFrm(nfrm))
            return FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
        vecCams.push_back(pscen->Pbkgd()->Icam());
        for (int32_t ipactr = 0; ipactr < cactr; ipactr++)
        {
            PACTR pactr;
            BRS xr, yr, zr;
            pglpactr->Get(ipactr, &pactr);
            pactr->Pbody()->GetPosition(&xr, &yr, &zr);
            // Frozen only matters for actions with more than one pose
            int32_t ccel = 0;
            pactr->Ptmpl()->FGetCcelActn(pactr->AnidCur(), &ccel);
            vecvecSamples[ipactr].push_back({pactr->FOnStage(), pactr->AnidCur(), pactr->FFrozen() && ccel > 1,
                                             stage.FromWorld(Vec3(DFromBrs(xr), DFromBrs(yr), DFromBrs(zr)))});
            if (pactr->FSoundInFrm())
                vecvecSoundFrames[ipactr].push_back(nfrm);
        }
        for (int32_t itbox = 0; itbox < pscen->Ctbox(); itbox++)
        {
            PTBOX ptbox = pscen->PtboxFromItbox(itbox);
            vecvecTboxVisible[itbox].push_back(ptbox != pvNil && ptbox->FIsVisible());
        }
        for (int32_t sty = stySfx; sty < styLim; sty++)
        {
            PGL pgltag = pvNil;
            int32_t vlm;
            bool fLoop;
            if (pscen->FQuerySnd(sty, &pgltag, &vlm, &fLoop) && pgltag != pvNil && pgltag->IvMac() > 0)
            {
                Json json = Json::MakeObject();
                json.Set("frame", nfrm);
                json.Set("type", sty == styMidi ? "music" : (sty == stySpeech ? "speech" : "sfx"));
                json.Set("loop", fLoop);
                jsonSceneSounds.Push(json);
            }
            ReleasePpo(&pgltag);
        }
    }

    // Cameras as ranges
    Json jsonCams = Json::MakeArray();
    for (size_t i = 0; i < vecCams.size(); i++)
    {
        if (i > 0 && vecCams[i] == vecCams[i - 1])
            continue;
        size_t j = i;
        while (j + 1 < vecCams.size() && vecCams[j + 1] == vecCams[i])
            j++;
        Json json = Json::MakeObject();
        json.Set("from", nfrmFirst + static_cast<int32_t>(i));
        json.Set("to", nfrmFirst + static_cast<int32_t>(j));
        json.Set("camera", vecCams[i]);
        jsonCams.Push(json);
    }

    // Actors as segments of constant (on stage, action, frozen)
    Json jsonActors = Json::MakeArray();
    for (int32_t ipactr = 0; ipactr < cactr; ipactr++)
    {
        PACTR pactr;
        STN stn;
        pglpactr->Get(ipactr, &pactr);
        const std::vector<ActorSample> &vec = vecvecSamples[ipactr];
        Json jsonSegs = Json::MakeArray();
        for (size_t i = 0; i < vec.size();)
        {
            size_t j = i;
            while (j + 1 < vec.size() && vec[j + 1].fOnStage == vec[i].fOnStage && vec[j + 1].anid == vec[i].anid &&
                   vec[j + 1].fFrozen == vec[i].fFrozen)
            {
                j++;
            }
            Json json = Json::MakeObject();
            json.Set("from", nfrmFirst + static_cast<int32_t>(i));
            json.Set("to", nfrmFirst + static_cast<int32_t>(j));
            if (!vec[i].fOnStage)
            {
                json.Set("off_stage", true);
            }
            else
            {
                json.Set("action", StrActionName(pactr->Ptmpl(), vec[i].anid));
                if (vec[i].fFrozen)
                    json.Set("frozen", true);
                double dMoved = (vec[j].stage - vec[i].stage).Length();
                if (dMoved > 0.05)
                {
                    json.Set("moves_from", _JsonXZ(vec[i].stage));
                    json.Set("moves_to", _JsonXZ(vec[j].stage));
                }
                else
                {
                    json.Set("at", _JsonXZ(vec[i].stage));
                }
            }
            jsonSegs.Push(json);
            i = j + 1;
        }
        Json jsonActor = Json::MakeObject();
        jsonActor.Set("id", pactr->Arid());
        if (pmvie->FGetName(pactr->Arid(), &stn))
            jsonActor.Set("name", StrFromStn(&stn));
        jsonActor.Set("segments", jsonSegs);
        if (!vecvecSoundFrames[ipactr].empty())
        {
            Json jsonFrames = Json::MakeArray();
            for (int32_t nfrm : vecvecSoundFrames[ipactr])
                jsonFrames.Push(nfrm);
            jsonActor.Set("sounds_start_at", jsonFrames);
        }
        jsonActors.Push(jsonActor);
    }

    Json jsonText = Json::MakeArray();
    for (int32_t itbox = 0; itbox < pscen->Ctbox(); itbox++)
    {
        const std::vector<bool> &vec = vecvecTboxVisible[itbox];
        Json jsonRanges = Json::MakeArray();
        for (size_t i = 0; i < vec.size(); i++)
        {
            if (!vec[i] || (i > 0 && vec[i - 1]))
                continue;
            size_t j = i;
            while (j + 1 < vec.size() && vec[j + 1])
                j++;
            Json json = Json::MakeObject();
            json.Set("from", nfrmFirst + static_cast<int32_t>(i));
            json.Set("to", nfrmFirst + static_cast<int32_t>(j));
            jsonRanges.Push(json);
        }
        Json json = Json::MakeObject();
        json.Set("index", itbox);
        PTBOX ptbox = pscen->PtboxFromItbox(itbox);
        json.Set("text", ptbox != pvNil ? _StrTboxText(ptbox).substr(0, 80) : "");
        json.Set("visible", jsonRanges);
        jsonText.Push(json);
    }

    pscen->FGotoFrm(nfrmRestore);
    if (iscenRestore != pmvie->Iscen())
        pmvie->FSwitchScen(iscenRestore);
    RefreshViews(pmvie);

    req.result.Set("scene_index", pmvie->Iscen());
    req.result.Set("first_frame", nfrmFirst);
    req.result.Set("last_frame", nfrmLast);
    req.result.Set("seconds", _Round2(static_cast<double>(nfrmLast - nfrmFirst + 1) / kfps));
    req.result.Set("positions_relative_to_camera", icamCoords);
    req.result.Set("cameras", jsonCams);
    req.result.Set("actors", jsonActors);
    req.result.Set("text_boxes", jsonText);
    req.result.Set("scene_sounds_start_at", jsonSceneSounds);
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
    {"preview_cameras", _FCmdPreviewCameras, fTrue},
    {"describe_timeline", _FCmdDescribeTimeline, fTrue},
    {"export_frames", _FCmdExportFrames, fTrue},
    {"play", _FCmdPlay, fTrue},
    {"stop", _FCmdStop, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
