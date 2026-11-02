/***************************************************************************
    Automation bridge: scene commands.
***************************************************************************/
#include "bridge.h"

ASSERTNAME

namespace automation
{

Json JsonMovieState(PMVIE pmvie);

static bool _FSetCamera(Request &req, PMVIE pmvie, int32_t icam)
{
    PBKGD pbkgd = pmvie->Pscen()->Pbkgd();
    if (icam < 0 || icam >= pbkgd->Ccam())
    {
        return req.Fail(kszErrBadArgs, "camera " + std::to_string(icam) + " does not exist; this background has cameras 0-" +
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
        {"cut", transCut},       {"fade_to_black", transFadeToBlack}, {"fade_to_white", transFadeToWhite},
        {"dissolve", transDissolve}, {"black", transBlack},
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
    return req.Fail(kszErrBadArgs,
                    "'transition' must be one of: cut, fade_to_black, fade_to_white, dissolve, black");
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

const CommandInfo krgcmdScene[] = {
    {"add_scene", _FCmdAddScene, fTrue},
    {"set_background", _FCmdSetBackground, fTrue},
    {"select_scene", _FCmdSelectScene, fTrue},
    {"set_camera", _FCmdSetCamera, fTrue},
    {"set_transition", _FCmdSetTransition, fTrue},
    {"delete_scene", _FCmdDeleteScene, fTrue},
    {"goto_frame", _FCmdGotoFrame, fTrue},
    {"add_pause", _FCmdAddPause, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
