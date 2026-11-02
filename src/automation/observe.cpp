/***************************************************************************
    Automation bridge: observing the movie (scene description, frame
    capture) and playback.
***************************************************************************/
#include "bridge.h"

#include <algorithm>
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
static PBKGD _PbkgdCur(void)
{
    PMVIE pmvie = PmvieCur();
    return pmvie != pvNil && pmvie->Pscen() != pvNil ? pmvie->Pscen()->Pbkgd() : pvNil;
}

// The palette for images of the current scene (or of pbkgdOverride's set)
static std::vector<PngColor> _PaletteCur(PBKGD pbkgdOverride = pvNil)
{
    std::vector<PngColor> palette;
    PGL pglclr = PglclrScene(pbkgdOverride != pvNil ? pbkgdOverride : _PbkgdCur());
    for (int32_t iclr = 0; pglclr != pvNil && iclr < pglclr->IvMac() && iclr < 256; iclr++)
    {
        CLR clr;
        pglclr->Get(iclr, &clr);
        palette.push_back({clr.bRed, clr.bGreen, clr.bBlue});
    }
    ReleasePpo(&pglclr);
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
    UseScenePalette(pmvie);
    PGPT pgpt = GPT::PgptNewOffscreen(&rc, 8);
    PGL pglclr = PglclrScene(pscen != pvNil ? pscen->Pbkgd() : pvNil);
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
        DrawLabel(pimg, x, y, strLabel, colors.bAxis, colors.bOutline, 1);
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

// Renders just the 3D world (no text boxes) for the view rect
static bool _FRenderWorld(PMVIE pmvie, RC *prcView, Image8 *pimg)
{
    PGPT pgpt = GPT::PgptNewOffscreen(prcView, 8);
    PGL pglclr = PglclrScene(pmvie->Pscen()->Pbkgd());
    bool fOk = pgpt != pvNil && pglclr != pvNil;
    if (fOk)
    {
        pgpt->SetOffscreenColors(pglclr);
        GNV gnv(pgpt);
        pmvie->Pbwld()->MarkDirty();
        pmvie->Pbwld()->Render();
        pmvie->Pbwld()->Draw(&gnv, prcView, 0, 0);
        _CopyPixels(pgpt, prcView, pimg);
    }
    ReleasePpo(&pgpt);
    ReleasePpo(&pglclr);
    return fOk;
}

// Counts the pixels in rc that change when pactr is hidden: its pixels
// that show in the current render
static bool _FCountActorPixels(PMVIE pmvie, PACTR pactr, RC *prcView, const RC &rc, int32_t *pcpix)
{
    Image8 imgWith, imgWithout;
    if (!_FRenderWorld(pmvie, prcView, &imgWith))
        return fFalse;
    pactr->Pbody()->Hide();
    bool fOk = _FRenderWorld(pmvie, prcView, &imgWithout);
    pactr->Pbody()->Show();
    if (!fOk)
        return fFalse;
    *pcpix = 0;
    for (int32_t y = rc.ypTop; y < rc.ypBottom; y++)
    {
        for (int32_t x = rc.xpLeft; x < rc.xpRight; x++)
            *pcpix += imgWith.Get(x, y) != imgWithout.Get(x, y);
    }
    return fTrue;
}

bool FMeanLuminance(PMVIE pmvie, RC rc, double *pdLuma)
{
    PMVU pmvu = PmvuCur();
    if (pmvu == pvNil || pmvie->Pscen() == pvNil)
        return fFalse;
    RC rcView, rcClip;
    pmvu->GetRc(&rcView, cooLocal);
    if (!rcClip.FIntersect(&rc, &rcView))
        return fFalse;
    UseScenePalette(pmvie);
    Image8 img;
    if (!_FRenderWorld(pmvie, &rcView, &img))
        return fFalse;
    std::vector<PngColor> palette = _PaletteCur();
    double dSum = 0;
    for (int32_t y = rcClip.ypTop; y < rcClip.ypBottom; y++)
    {
        for (int32_t x = rcClip.xpLeft; x < rcClip.xpRight; x++)
        {
            const PngColor &clr = palette[img.Get(x, y) % palette.size()];
            dSum += (0.299 * clr.red + 0.587 * clr.green + 0.114 * clr.blue) / 255.0;
        }
    }
    *pdLuma = dSum / LwMax(1, rcClip.Dxp() * rcClip.Dyp());
    return fTrue;
}

bool FVisibleFraction(PMVIE pmvie, PACTR pactr, double *pdFraction)
{
    PMVU pmvu = PmvuCur();
    PSCEN pscen = pmvie->Pscen();
    if (pmvu == pvNil || pscen == pvNil || !pactr->FOnStage())
        return fFalse;
    RC rcView, rcActr, rcBox;
    pmvu->GetRc(&rcView, cooLocal);
    UseScenePalette(pmvie);
    EnsureRendered(pmvie);
    pactr->GetRcBounds(&rcActr);
    if (!rcBox.FIntersect(&rcActr, &rcView))
    {
        *pdFraction = 0;
        return fTrue;
    }

    // What shows now, against its whole silhouette: the same with nothing
    // in front of it (no background depth, other actors hidden)
    int32_t cpixVisible, cpixWhole = 0;
    bool fOk = _FCountActorPixels(pmvie, pactr, &rcView, rcBox, &cpixVisible);
    if (fOk)
    {
        std::vector<PACTR> vecHidden;
        PGL pglpactr = pscen->PglRollCall();
        for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
        {
            PACTR pactrOther;
            pglpactr->Get(ipactr, &pactrOther);
            if (pactrOther != pactr && pactrOther->FOnStage())
            {
                pactrOther->Pbody()->Hide();
                vecHidden.push_back(pactrOther);
            }
        }
        pmvie->Pbwld()->SetIgnoreBackgroundDepth(fTrue);
        fOk = _FCountActorPixels(pmvie, pactr, &rcView, rcBox, &cpixWhole);
        pmvie->Pbwld()->SetIgnoreBackgroundDepth(fFalse);
        for (PACTR pactrOther : vecHidden)
            pactrOther->Pbody()->Show();
    }
    EnsureRendered(pmvie);
    if (!fOk)
        return fFalse;
    *pdFraction = cpixWhole == 0 ? 0 : LwMin(cpixVisible, cpixWhole) / static_cast<double>(cpixWhole);
    return fTrue;
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
        DrawLabel(&imgSmall, 4, 4, "CAM " + std::to_string(vecCams[i]), bText, bOutline, 2);
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

/***************************************************************************
    Storyboard: frames sampled across a scene on one labelled image
***************************************************************************/
const int32_t kcStoryboardMax = 24;

struct StoryCell
{
    int32_t iscen;
    int32_t nfrm;
    double dSec; // into the scene, or into the movie for a whole-movie board
};

// Spreads ccell cells over every scene in proportion to its length (at
// least one each), evenly within each scene
static std::vector<StoryCell> _VecMovieCells(PMVIE pmvie, int32_t ccell)
{
    std::vector<int32_t> vecFirst, vecLen;
    int32_t cfrmTotal = 0;
    for (int32_t iscen = 0; iscen < pmvie->Cscen(); iscen++)
    {
        if (iscen != pmvie->Iscen() && !pmvie->FSwitchScen(iscen))
            return {};
        vecFirst.push_back(pmvie->Pscen()->NfrmFirst());
        vecLen.push_back(pmvie->Pscen()->NfrmLast() - pmvie->Pscen()->NfrmFirst() + 1);
        cfrmTotal += vecLen.back();
    }
    std::vector<StoryCell> vecCells;
    int32_t cfrmBefore = 0;
    for (int32_t iscen = 0; iscen < pmvie->Cscen(); iscen++)
    {
        int32_t ccellScen =
            LwBound(static_cast<int32_t>(std::lround(ccell * static_cast<double>(vecLen[iscen]) / LwMax(1, cfrmTotal))),
                    1, vecLen[iscen] + 1);
        for (int32_t i = 0; i < ccellScen; i++)
        {
            // Centre the samples in equal slices so a scene's cells don't
            // all land on its first and last frames
            int32_t dnfrm = static_cast<int32_t>((i + 0.5) * vecLen[iscen] / ccellScen);
            vecCells.push_back({iscen, vecFirst[iscen] + dnfrm, (cfrmBefore + dnfrm) / static_cast<double>(kfps)});
        }
        cfrmBefore += vecLen[iscen];
    }
    return vecCells;
}

static bool _FCmdStoryboard(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    std::string strPath;
    int32_t cframe = 12;
    bool fAllScenes = fFalse;
    static int32_t _cStoryboard = 0;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgInt("count", &cframe, fFalse) || !req.FArgString("path", &strPath, fFalse) ||
        !req.FArgBool("all_scenes", &fAllScenes, fFalse))
    {
        return fFalse;
    }
    cframe = LwBound(cframe, 2, kcStoryboardMax + 1);

    int32_t iscenRestore = pmvie->Iscen();
    int32_t nfrmRestore = pscen->Nfrm();
    std::vector<StoryCell> vecCells;
    if (fAllScenes)
    {
        vecCells = _VecMovieCells(pmvie, cframe);
        if (vecCells.empty())
            return FFailEngine(req, "could not read the scenes");
        if (static_cast<int32_t>(vecCells.size()) > kcStoryboardMax)
            vecCells.resize(kcStoryboardMax); // more scenes than cells: the board shows the first ones
    }
    else
    {
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
        int32_t nfrmFrom = pscen->NfrmFirst(), nfrmTo = pscen->NfrmLast();
        if (!req.FArgInt("from", &nfrmFrom, fFalse) || !req.FArgInt("to", &nfrmTo, fFalse))
            return fFalse;
        nfrmFrom = LwMax(nfrmFrom, pscen->NfrmFirst());
        nfrmTo = LwMin(nfrmTo, pscen->NfrmLast());
        if (nfrmTo < nfrmFrom)
            return req.Fail(kszErrBadArgs, "empty frame range");
        cframe = LwMin(cframe, nfrmTo - nfrmFrom + 1);
        for (int32_t i = 0; i < cframe; i++)
        {
            int32_t nfrm = cframe == 1 ? nfrmFrom : nfrmFrom + LwMulDiv(nfrmTo - nfrmFrom, i, cframe - 1);
            vecCells.push_back({pmvie->Iscen(), nfrm, static_cast<double>(nfrm - pscen->NfrmFirst()) / kfps});
        }
    }

    // The sheet uses the palette of the first cell's set; cells from sets
    // with other palettes are mapped to the nearest colours
    std::vector<PngColor> palette;
    uint8_t bText = 0, bOutline = 0;
    Image8 imgSheet;
    Json jsonFrames = Json::MakeArray();
    int32_t ccell = static_cast<int32_t>(vecCells.size());
    for (int32_t i = 0; i < ccell && !req.Failed(); i++)
    {
        const StoryCell &cell = vecCells[i];
        if (cell.iscen != pmvie->Iscen() && !pmvie->FSwitchScen(cell.iscen))
        {
            FFailEngine(req, "could not switch to scene " + std::to_string(cell.iscen));
            break;
        }
        pscen = pmvie->Pscen();
        Image8 img;
        if (!pscen->FGotoFrm(cell.nfrm))
        {
            FFailEngine(req, "could not go to frame " + std::to_string(cell.nfrm));
            break;
        }
        if (!_FCaptureView(req, &img))
            break;
        std::vector<PngColor> paletteCell = _PaletteCur();
        if (palette.empty())
        {
            palette = paletteCell;
            bText = BNearestColor(palette, 255, 240, 0);
            bOutline = BNearestColor(palette, 0, 0, 0);
        }
        else if (!std::equal(palette.begin(), palette.end(), paletteCell.begin(), paletteCell.end(),
                             [](const PngColor &a, const PngColor &b) {
                                 return a.red == b.red && a.green == b.green && a.blue == b.blue;
                             }))
        {
            uint8_t rgbMap[256] = {};
            for (size_t iclr = 0; iclr < paletteCell.size() && iclr < 256; iclr++)
                rgbMap[iclr] =
                    BNearestColor(palette, paletteCell[iclr].red, paletteCell[iclr].green, paletteCell[iclr].blue);
            for (uint8_t &b : img.pixels)
                b = rgbMap[b];
        }
        Image8 imgSmall = Downscale2x(img);
        if (imgSheet.width == 0)
        {
            int32_t crow = (ccell + kcColumnsPreview - 1) / kcColumnsPreview;
            int32_t ccol = LwMin(kcColumnsPreview, ccell);
            imgSheet = Image8(ccol * imgSmall.width + (ccol - 1) * kdzpGapPreview,
                              crow * imgSmall.height + (crow - 1) * kdzpGapPreview, bOutline);
        }
        char rgch[40];
        if (fAllScenes)
            snprintf(rgch, sizeof(rgch), "S%d F%d  %.1fS", cell.iscen, cell.nfrm, cell.dSec);
        else
            snprintf(rgch, sizeof(rgch), "F%d  %.1fS", cell.nfrm, cell.dSec);
        DrawLabel(&imgSmall, 4, 4, rgch, bText, bOutline, 2);
        int32_t col = i % kcColumnsPreview, row = i / kcColumnsPreview;
        Blit(&imgSheet, imgSmall, col * (imgSmall.width + kdzpGapPreview), row * (imgSmall.height + kdzpGapPreview));
        if (fAllScenes)
        {
            Json json = Json::MakeObject();
            json.Set("scene", cell.iscen);
            json.Set("frame", cell.nfrm);
            json.Set("seconds", std::round(cell.dSec * 10) / 10);
            jsonFrames.Push(json);
        }
        else
        {
            jsonFrames.Push(cell.nfrm);
        }
    }

    if (iscenRestore != pmvie->Iscen())
        pmvie->FSwitchScen(iscenRestore);
    pmvie->Pscen()->FGotoFrm(nfrmRestore);
    RefreshViews(pmvie);
    if (req.Failed())
        return fFalse;

    if (strPath.empty())
        strPath = _StrJoin(_StrDefaultCaptureDir(), "3dmm-storyboard-" + std::to_string(++_cStoryboard) + ".png");
    if (!_FWriteImage(req, strPath, imgSheet, palette))
        return fFalse;
    req.result.Set("path", strPath);
    req.result.Set("frames", jsonFrames);
    req.result.Set("columns", kcColumnsPreview);
    req.result.Set("note", fAllScenes ? "cells are labelled S<scene> F<frame> and seconds into the movie"
                                      : "cells are labelled with the frame number and seconds into the scene");
    return fTrue;
}

/***************************************************************************
    Camera suggestions: which of the set's cameras show the actors best
***************************************************************************/
static bool _FCmdSuggestCamera(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie) || !FApplyFrameArg(req, pscen))
        return fFalse;

    // Which actors to frame: the listed ones, or everyone on stage
    std::vector<PACTR> vecActors;
    const Json &jsonActors = req.args.Get("actors");
    if (jsonActors.IsArray() && jsonActors.Size() > 0)
    {
        for (const Json &json : jsonActors.AsArray())
        {
            Request reqT;
            reqT.args.Set("actor", json);
            PACTR pactr;
            if (!FResolveActor(reqT, "actor", &pactr))
                return req.Fail(reqT.errCode.c_str(), reqT.errMessage);
            vecActors.push_back(pactr);
        }
    }
    else
    {
        PGL pglpactr = pscen->PglRollCall();
        for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
        {
            PACTR pactr;
            pglpactr->Get(ipactr, &pactr);
            if (pactr->FOnStage())
                vecActors.push_back(pactr);
        }
    }
    if (vecActors.empty())
        return req.Fail(kszErrBadState, "no actors on stage at this frame to frame");

    PBKGD pbkgd = pscen->Pbkgd();
    int32_t icamRestore = pbkgd->Icam();
    RC rcView(0, 0, 544, 306);
    if (PmvuCur() != pvNil)
        PmvuCur()->GetRc(&rcView, cooLocal);
    struct Rating
    {
        int32_t icam;
        int32_t cin;
        double dyAverage;
        double dxCentre;
        Json jsonActors;
    };
    std::vector<Rating> vecRatings;
    for (int32_t icam = 0; icam < pbkgd->Ccam(); icam++)
    {
        if (!pbkgd->FSetCamera(pmvie->Pbwld(), icam))
            continue;
        EnsureRendered(pmvie);
        Rating rating{icam, 0, 0, 0, Json::MakeArray()};
        double dxSum = 0;
        for (PACTR pactr : vecActors)
        {
            STN stn;
            RC rc;
            bool fIn = pactr->FOnStage() && pactr->FIsInView();
            Json json = Json::MakeObject();
            json.Set("id", pactr->Arid());
            if (pmvie->FGetName(pactr->Arid(), &stn))
                json.Set("name", StrFromStn(&stn));
            json.Set("in_view", fIn);
            if (fIn)
            {
                pactr->GetRcBounds(&rc);
                // Partly off screen counts as out of shot
                bool fWhole = rc.xpLeft > 0 && rc.xpRight < rcView.Dxp() && rc.ypTop > 0 && rc.ypBottom < rcView.Dyp();
                json.Set("fully_in_shot", fWhole);
                json.Set("screen_height", rc.Dyp());
                if (fWhole)
                {
                    rating.cin++;
                    rating.dyAverage += rc.Dyp();
                    dxSum += (rc.xpLeft + rc.xpRight) / 2.0;
                }
            }
            rating.jsonActors.Push(json);
        }
        if (rating.cin > 0)
        {
            rating.dyAverage /= rating.cin;
            rating.dxCentre = std::fabs(dxSum / rating.cin - rcView.Dxp() / 2.0);
        }
        vecRatings.push_back(rating);
    }
    pbkgd->FSetCamera(pmvie->Pbwld(), icamRestore);
    RefreshViews(pmvie);

    // Most actors fully in shot first; then actors that fill the frame well
    // (around half the frame height), then the best-centred group
    std::sort(vecRatings.begin(), vecRatings.end(), [](const Rating &a, const Rating &b) {
        if (a.cin != b.cin)
            return a.cin > b.cin;
        // About half the frame height reads well for a group
        double da = std::fabs(a.dyAverage - 150), db = std::fabs(b.dyAverage - 150);
        if (std::fabs(da - db) > 10)
            return da < db;
        return a.dxCentre < b.dxCentre;
    });
    Json jsonList = Json::MakeArray();
    for (const Rating &rating : vecRatings)
    {
        Json json = Json::MakeObject();
        json.Set("camera", rating.icam);
        json.Set("actors_fully_in_shot", rating.cin);
        json.Set("average_screen_height", static_cast<int>(rating.dyAverage));
        json.Set("actors", rating.jsonActors);
        jsonList.Push(json);
    }
    req.result.Set("frame", pscen->Nfrm());
    req.result.Set("actors_considered", static_cast<int>(vecActors.size()));
    req.result.Set("cameras", jsonList);
    // Framing advice for the best camera
    if (!vecRatings.empty() && vecRatings[0].cin > 0)
    {
        const Rating &best = vecRatings[0];
        int32_t dypView = rcView.Dyp();
        if (best.dyAverage < dypView / 3)
        {
            req.result.Set("framing", "even on the best camera the actors look small (about " +
                                          std::to_string(static_cast<int>(best.dyAverage)) + " of " +
                                          std::to_string(dypView) +
                                          " px tall); move them closer to the camera (lower z) with place_actor");
        }
        else if (best.dyAverage > dypView * 4 / 5)
        {
            req.result.Set("framing", "the actors nearly fill the frame; move them further away (higher z) for a "
                                      "wider shot");
        }
    }
    req.result.Set("note", "ranked best first; doesn't know about furniture blocking the view, so check the pick with "
                           "render_frame or preview_cameras. Apply with set_camera.");
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

std::string StrTboxText(PTBOX ptbox)
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
        Json jsonActor = JsonActor(pmvie, pactr, stage, camera);
        double dVisible;
        if (pactr->FOnStage() && pactr->FIsInView() && FVisibleFraction(pmvie, pactr, &dVisible))
            jsonActor.Set("visible_fraction", std::round(dVisible * 100) / 100);
        jsonActors.Push(jsonActor);
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
        json.Set("text", StrTboxText(ptbox));
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
        json.Set("text", ptbox != pvNil ? StrTboxText(ptbox).substr(0, 80) : "");
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
    if (req.MsElapsed() > LwMin(static_cast<int32_t>(dSecMax * kdtsSecond), kdtsPlayTimeoutMax))
    {
        pmvie->Play(); // stop
        req.result.Set("finished", false);
        req.result.Set("note", "stopped after the timeout (" + std::to_string(req.MsElapsed() / 1000) + " s)");
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
        req.poll = [fStarted = false](Request &req) mutable {
            PMVIE pmvie = PmvieCur();
            if (pmvie != pvNil && pmvie->FPlaying())
                fStarted = true;
            else if (!fStarted)
            {
                if (req.MsElapsed() < kdtsSecond)
                    return false;
                // Short movies can finish between polls; only a playhead
                // that never moved means playback didn't start
                if (pmvie != pvNil && pmvie->Iscen() == req.result.Get("started_scene").AsNumber() &&
                    pmvie->Pscen()->Nfrm() == req.result.Get("started_frame").AsNumber())
                {
                    req.result.Set("finished", false);
                    req.result.Set("note", "playback did not start");
                    return true;
                }
            }
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
    {"storyboard", _FCmdStoryboard, fTrue},
    {"suggest_camera", _FCmdSuggestCamera, fTrue},
    {"export_frames", _FCmdExportFrames, fTrue},
    {"play", _FCmdPlay, fTrue},
    {"stop", _FCmdStop, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
