/***************************************************************************
    Automation bridge: scene commands.
***************************************************************************/
#include "bridge.h"

#include <filesystem>

ASSERTNAME

namespace automation
{

Json JsonMovieState(PMVIE pmvie);

static bool _FSetCamera(Request &req, PMVIE pmvie, int32_t icam)
{
    PBKGD pbkgd = pmvie->Pscen()->Pbkgd();
    if (icam < 0 || icam >= pbkgd->Ccam())
    {
        return req.Fail(kszErrBadArgs, "camera " + std::to_string(icam) +
                                           " does not exist; this background has cameras 0-" +
                                           std::to_string(pbkgd->Ccam() - 1));
    }
    if (icam == pbkgd->Icam())
        return fTrue;
    if (!pmvie->FChangeCam(icam))
        return FFailEngine(req, "could not change camera");
    return fTrue;
}

static bool _FCmdAddScene(Request &req)
{
    PMVIE pmvie;
    std::string strBkgd;
    const CatalogEntry *pentry;
    int32_t icam = 0;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("background", &strBkgd) || !req.FArgInt("camera", &icam, fFalse))
        return fFalse;
    if (!FFindContent(req, strBkgd, {ContentKind::Background}, &pentry))
        return fFalse;

    bool fReplaced = pmvie->Pscen() != pvNil && pmvie->Pscen()->FIsEmpty();
    TAG tag = pentry->tag;
    if (!pmvie->FAddScen(&tag))
        return FFailEngine(req, "could not add a scene with background '" + pentry->name + "'");

    if (!_FSetCamera(req, pmvie, icam))
        return fFalse;

    RefreshViews(pmvie);
    req.result.Set("scene_index", pmvie->Iscen());
    req.result.Set("replaced_empty_scene", fReplaced);
    if (fReplaced)
    {
        req.result.Set("note", "the current scene was still empty, so (as in the Studio) its background was changed "
                               "instead of adding another scene");
    }
    req.result.Set("movie", JsonMovieState(pmvie));
    return fTrue;
}

static bool _FCmdSetBackground(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    std::string strBkgd;
    const CatalogEntry *pentry;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("background", &strBkgd))
        return fFalse;
    if (!FFindContent(req, strBkgd, {ContentKind::Background}, &pentry))
        return fFalse;
    TAG tag = pentry->tag;
    if (!BKGD::FCacheToHD(&tag) || !pscen->FSetBkgd(&tag))
        return FFailEngine(req, "could not set background '" + pentry->name + "'");
    RefreshViews(pmvie);
    req.result = JsonMovieState(pmvie);
    return fTrue;
}

static bool _FCmdSelectScene(Request &req)
{
    PMVIE pmvie;
    int32_t iscen;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie) || !req.FArgInt("index", &iscen))
        return fFalse;
    if (iscen < 0 || iscen >= pmvie->Cscen())
    {
        return req.Fail(kszErrBadArgs, "scene " + std::to_string(iscen) + " does not exist (the movie has " +
                                           std::to_string(pmvie->Cscen()) + " scenes, numbered from 0)");
    }
    if (iscen != pmvie->Iscen() && !pmvie->FSwitchScen(iscen))
        return FFailEngine(req, "could not switch to scene " + std::to_string(iscen));
    if (!FApplyFrameArg(req, pmvie->Pscen()))
        return fFalse;
    RefreshViews(pmvie);
    req.result = JsonMovieState(pmvie);
    return fTrue;
}

static bool _FCmdSetCamera(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    int32_t icam;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie) || !req.FArgInt("camera", &icam))
        return fFalse;
    if (!FApplyFrameArg(req, pscen) || !_FSetCamera(req, pmvie, icam))
        return fFalse;
    RefreshViews(pmvie);
    req.result.Set("camera", pscen->Pbkgd()->Icam());
    req.result.Set("frame", pscen->Nfrm());
    return fTrue;
}

static bool _FCmdSetTransition(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    std::string strTrans;
    static const struct
    {
        const char *psz;
        TRANS trans;
    } _rgtrans[] = {
        {"cut", transCut},
        {"fade_to_black", transFadeToBlack},
        {"fade_to_white", transFadeToWhite},
        {"dissolve", transDissolve},
        {"black", transBlack},
    };

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie) ||
        !req.FArgString("transition", &strTrans))
    {
        return fFalse;
    }
    strTrans = StrLower(strTrans);
    for (auto &entry : _rgtrans)
    {
        if (strTrans == entry.psz)
        {
            if (!pmvie->FSetTransition(entry.trans))
                return FFailEngine(req, "could not set the transition");
            req.result.Set("scene_index", pmvie->Iscen());
            req.result.Set("transition", entry.psz);
            return fTrue;
        }
    }
    return req.Fail(kszErrBadArgs, "'transition' must be one of: cut, fade_to_black, fade_to_white, dissolve, black");
}

static bool _FCmdDeleteScene(Request &req)
{
    PMVIE pmvie;
    int32_t iscen;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    iscen = pmvie->Iscen();
    if (!req.FArgInt("index", &iscen, fFalse))
        return fFalse;
    if (iscen == ivNil || iscen < 0 || iscen >= pmvie->Cscen())
        return req.Fail(kszErrBadArgs, "scene " + std::to_string(iscen) + " does not exist");
    if (!pmvie->FRemScen(iscen))
        return FFailEngine(req, "could not delete scene " + std::to_string(iscen));
    RefreshViews(pmvie);
    req.result = JsonMovieState(pmvie);
    return fTrue;
}

static bool _FCmdGotoFrame(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    int32_t nfrm;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (req.FHasArg("frame"))
    {
        if (!req.FArgInt("frame", &nfrm))
            return fFalse;
    }
    else
    {
        std::string strWhere;
        if (!req.FArgString("where", &strWhere))
            return req.Fail(kszErrBadArgs, "pass 'frame' (a number) or 'where' (\"start\" or \"end\")");
        strWhere = StrLower(strWhere);
        if (strWhere == "start" || strWhere == "first")
            nfrm = pscen->NfrmFirst();
        else if (strWhere == "end" || strWhere == "last")
            nfrm = pscen->NfrmLast();
        else
            return req.Fail(kszErrBadArgs, "'where' must be \"start\" or \"end\"");
    }
    if (!FGotoFrame(req, pscen, nfrm))
        return fFalse;
    RefreshViews(pmvie);
    req.result.Set("frame", pscen->Nfrm());
    req.result.Set("first_frame", pscen->NfrmFirst());
    req.result.Set("last_frame", pscen->NfrmLast());
    return fTrue;
}

static bool _FCmdAddPause(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    std::string strKind;
    WIT wit;
    int32_t dts = 0;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (!req.FArgString("kind", &strKind) || !FApplyFrameArg(req, pscen))
        return fFalse;
    strKind = StrLower(strKind);
    if (strKind == "seconds" || strKind == "time")
    {
        double dSec;
        if (!req.FArgNumber("seconds", &dSec))
            return fFalse;
        if (dSec <= 0 || dSec > 600)
            return req.Fail(kszErrBadArgs, "'seconds' must be between 0 and 600");
        wit = witForTime;
        dts = static_cast<int32_t>(dSec * kdtimSecond);
    }
    else if (strKind == "until_sound")
    {
        wit = witUntilSnd;
    }
    else if (strKind == "until_click")
    {
        wit = witUntilClick;
    }
    else if (strKind == "none" || strKind == "remove")
    {
        wit = witNil;
    }
    else
    {
        return req.Fail(kszErrBadArgs, "'kind' must be \"seconds\", \"until_sound\", \"until_click\" or \"remove\"");
    }
    if (!pmvie->FPause(wit, dts))
        return FFailEngine(req, "could not add the pause");
    req.result.Set("frame", pscen->Nfrm());
    req.result.Set("kind", strKind);
    return fTrue;
}

/***************************************************************************
    Scene editing: reorder, import, duplicate, trim

    Reordering and importing go through the same "composite movie" (CMVI)
    the Studio's scene sorter uses: MVIE::FAddToCmvi lists a movie's scenes
    (another movie's scenes are appended as imports), the list is edited,
    and MVIE::FSetCmvi rebuilds the movie from it.
***************************************************************************/
bool FFniFromPath(Request &req, const std::string &strPathIn, FTG ftg, FNI *pfni);

// Makes sure a scene is open after FSetCmvi, which may close the current one
static void _EnsureSceneOpen(PMVIE pmvie, int32_t iscenWant)
{
    if (pmvie->Cscen() == 0)
        return;
    int32_t iscen = LwBound(iscenWant, 0, pmvie->Cscen());
    if (pmvie->Pscen() == pvNil || pmvie->Iscen() != iscen)
        pmvie->FSwitchScen(iscen);
    pmvie->InvalViewsAndScb();
    pmvie->Pmcc()->SceneChange();
}

static bool _FApplyCmvi(Request &req, PMVIE pmvie, CMVI *pcmvi, int32_t iscenOpen, const char *pszWhat)
{
    bool fOk = pmvie->FSetCmvi(pcmvi);
    pcmvi->Empty();
    if (!fOk)
        return FFailEngine(req, std::string("could not ") + pszWhat);
    pmvie->SetDirty();
    _EnsureSceneOpen(pmvie, iscenOpen);
    return fTrue;
}

static bool _FCmdMoveScene(Request &req)
{
    PMVIE pmvie;
    int32_t iscenFrom, iscenTo;
    CMVI cmvi;
    int32_t iscendIns = 0;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie) || !req.FArgInt("from", &iscenFrom) ||
        !req.FArgInt("to", &iscenTo))
    {
        return fFalse;
    }
    int32_t cscen = pmvie->Cscen();
    if (iscenFrom < 0 || iscenFrom >= cscen || iscenTo < 0 || iscenTo >= cscen)
        return req.Fail(kszErrBadArgs, "scene indexes must be 0-" + std::to_string(cscen - 1));
    if (iscenFrom == iscenTo)
    {
        req.result = JsonMovieState(pmvie);
        return fTrue;
    }

    cmvi.pglmvied = cmvi.pglscend = pvNil;
    if (!pmvie->FAddToCmvi(&cmvi, &iscendIns))
    {
        cmvi.Empty();
        return FFailEngine(req, "could not read the scene list");
    }
    // GL::Move inserts before the target's original position
    cmvi.pglscend->Move(iscenFrom, iscenTo > iscenFrom ? iscenTo + 1 : iscenTo);
    if (!_FApplyCmvi(req, pmvie, &cmvi, iscenTo, "reorder the scenes"))
        return fFalse;
    req.result = JsonMovieState(pmvie);
    return fTrue;
}

// Appends scenes of the movie at fni (all, or the listed indexes) and moves
// them to position iscenAt. Returns the index of the first inserted scene.
static bool _FImportScenes(Request &req, PMVIE pmvie, FNI *pfni, const std::vector<int32_t> &vecWant, int32_t iscenAt,
                           int32_t *pcscenAdded)
{
    CMVI cmvi;
    int32_t iscendIns = 0;
    MCC mcc(2, 2, 0);

    cmvi.pglmvied = cmvi.pglscend = pvNil;
    if (!pmvie->FAddToCmvi(&cmvi, &iscendIns))
    {
        cmvi.Empty();
        return FFailEngine(req, "could not read the scene list");
    }
    int32_t cscenOwn = cmvi.pglscend->IvMac();

    PMVIE pmvieOther = MVIE::PmvieNew(vpapp->FSlowCPU(), &mcc, pfni, cnoNil);
    if (pmvieOther == pvNil)
    {
        cmvi.Empty();
        return FFailEngine(req, "could not open the movie to import from");
    }
    bool fOk = pmvieOther->FAddToCmvi(&cmvi, &iscendIns);
    int32_t cscenOther = pmvieOther->Cscen();
    ReleasePpo(&pmvieOther);
    if (!fOk)
    {
        cmvi.Empty();
        return FFailEngine(req, "could not read the other movie's scenes");
    }

    for (int32_t iscen : vecWant)
    {
        if (iscen < 0 || iscen >= cscenOther)
        {
            cmvi.Empty();
            return req.Fail(kszErrBadArgs, "the other movie has scenes 0-" + std::to_string(cscenOther - 1));
        }
    }

    // Drop the imported scenes that weren't asked for, then move the rest
    // into place, keeping their order
    int32_t cscenAdded = 0;
    for (int32_t iscen = 0; iscen < cscenOther; iscen++)
    {
        bool fWant = vecWant.empty();
        for (int32_t iscenWant : vecWant)
            fWant |= iscenWant == iscen;
        SCEND scend;
        cmvi.pglscend->Get(cscenOwn + iscen, &scend);
        if (!fWant)
        {
            scend.fNuked = fTrue;
            cmvi.pglscend->Put(cscenOwn + iscen, &scend);
        }
    }
    // Nuked entries are skipped by FSetCmvi; move every wanted entry
    // (in order) to iscenAt + its rank
    for (int32_t iscen = 0; iscen < cscenOther; iscen++)
    {
        SCEND scend;
        int32_t iscend = cscenOwn + iscen;
        cmvi.pglscend->Get(iscend, &scend);
        if (scend.fNuked)
            continue;
        cmvi.pglscend->Move(iscend, iscenAt + cscenAdded);
        cscenAdded++;
    }

    if (!_FApplyCmvi(req, pmvie, &cmvi, iscenAt, "import the scenes"))
        return fFalse;
    *pcscenAdded = cscenAdded;
    return fTrue;
}

static bool _FCmdImportScenes(Request &req)
{
    PMVIE pmvie;
    std::string strPath;
    FNI fni;
    std::vector<int32_t> vecWant;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie) || !req.FArgString("path", &strPath))
        return fFalse;
    if (!FFniFromPath(req, strPath, kftg3mm, &fni))
        return fFalse;
    if (fni.TExists() != tYes)
        return req.Fail(kszErrIo, "no such file: " + strPath);
    const Json &jsonScenes = req.args.Get("scenes");
    if (jsonScenes.IsArray())
    {
        for (const Json &json : jsonScenes.AsArray())
        {
            if (!json.IsNumber())
                return req.Fail(kszErrBadArgs, "'scenes' must be a list of scene indexes");
            vecWant.push_back(static_cast<int32_t>(json.AsNumber()));
        }
    }
    int32_t iscenAt = pmvie->Cscen();
    if (!req.FArgInt("at", &iscenAt, fFalse))
        return fFalse;
    iscenAt = LwBound(iscenAt, 0, pmvie->Cscen() + 1);

    int32_t cscenAdded = 0;
    if (!_FImportScenes(req, pmvie, &fni, vecWant, iscenAt, &cscenAdded))
        return fFalse;
    req.result = JsonMovieState(pmvie);
    req.result.Set("scenes_added", cscenAdded);
    req.result.Set("first_added_scene", iscenAt);
    return fTrue;
}

static bool _FCmdDuplicateScene(Request &req)
{
    PMVIE pmvie;
    int32_t iscen;
    FNI fni;

    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    iscen = pmvie->Iscen();
    if (!req.FArgInt("index", &iscen, fFalse))
        return fFalse;
    if (iscen == ivNil || iscen < 0 || iscen >= pmvie->Cscen())
        return req.Fail(kszErrBadArgs, "scene " + std::to_string(iscen) + " does not exist");

    // Snapshot the movie (the Studio's Save a Copy, which leaves this movie
    // as it is) and import the scene back from the snapshot
    std::error_code ec;
    std::filesystem::path path = std::filesystem::temp_directory_path(ec);
    if (ec)
        path = ".";
    path /= "3dmm-duplicate-" + std::to_string(TsCurrent());
    std::filesystem::create_directories(path, ec);
    std::string strTitle = StrFromStn(pmvie->PstnTitle());
    for (char &ch : strTitle)
    {
        if (ch == '/' || ch == '\\' || ch == ':')
            ch = '-';
    }
    path /= (strTitle.empty() ? std::string("movie") : strTitle) + ".3mm";
    if (!FFniFromPath(req, path.string(), kftg3mm, &fni))
        return fFalse;
    if (!pmvie->FSaveToFni(&fni, fFalse))
        return FFailEngine(req, "could not snapshot the movie");
    pmvie->SetDirty(fTrue);

    int32_t cscenAdded = 0;
    bool fOk = _FImportScenes(req, pmvie, &fni, {iscen}, iscen + 1, &cscenAdded);
    std::filesystem::remove_all(path.parent_path(), ec);
    if (!fOk)
        return fFalse;
    req.result = JsonMovieState(pmvie);
    req.result.Set("new_scene_index", iscen + 1);
    req.result.Set("note", "the copy has the same actors (new ids); edit it independently");
    return fTrue;
}

static bool _FCmdTrimScene(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    int32_t nfrmKeepFirst, nfrmKeepLast;

    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    nfrmKeepFirst = pscen->NfrmFirst();
    nfrmKeepLast = pscen->NfrmLast();
    if (!req.FArgInt("keep_from", &nfrmKeepFirst, fFalse) || !req.FArgInt("keep_to", &nfrmKeepLast, fFalse))
        return fFalse;
    if (!req.FHasArg("keep_from") && !req.FHasArg("keep_to"))
        return req.Fail(kszErrBadArgs, "pass 'keep_from' and/or 'keep_to' (frame numbers to keep)");
    if (nfrmKeepFirst > nfrmKeepLast || nfrmKeepFirst < pscen->NfrmFirst() || nfrmKeepLast > pscen->NfrmLast())
    {
        return req.Fail(kszErrBadArgs, "the kept range must lie within " + std::to_string(pscen->NfrmFirst()) + "-" +
                                           std::to_string(pscen->NfrmLast()));
    }

    // The Studio's chop tools: everything after the current frame, then
    // everything before it
    if (nfrmKeepLast < pscen->NfrmLast())
    {
        if (!pscen->FGotoFrm(nfrmKeepLast) || !pscen->FChop())
            return FFailEngine(req, "could not cut the end of the scene");
    }
    if (nfrmKeepFirst > pscen->NfrmFirst())
    {
        if (!pscen->FGotoFrm(nfrmKeepFirst) || !pscen->FChopBack())
            return FFailEngine(req, "could not cut the start of the scene");
    }
    pmvie->SetDirty();
    RefreshViews(pmvie);
    req.result.Set("first_frame", pscen->NfrmFirst());
    req.result.Set("last_frame", pscen->NfrmLast());
    req.result.Set("seconds", static_cast<double>(pscen->NfrmLast() - pscen->NfrmFirst() + 1) / kfps);
    req.result.Set("note", "frame numbers may have shifted; use describe_timeline to see the result");
    return fTrue;
}

const CommandInfo krgcmdScene[] = {
    {"add_scene", _FCmdAddScene, fTrue},
    {"set_background", _FCmdSetBackground, fTrue},
    {"select_scene", _FCmdSelectScene, fTrue},
    {"set_camera", _FCmdSetCamera, fTrue},
    {"set_transition", _FCmdSetTransition, fTrue},
    {"delete_scene", _FCmdDeleteScene, fTrue},
    {"move_scene", _FCmdMoveScene, fTrue},
    {"duplicate_scene", _FCmdDuplicateScene, fTrue},
    {"import_scenes", _FCmdImportScenes, fTrue},
    {"trim_scene", _FCmdTrimScene, fTrue},
    {"goto_frame", _FCmdGotoFrame, fTrue},
    {"add_pause", _FCmdAddPause, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
