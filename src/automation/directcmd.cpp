/***************************************************************************
    Automation bridge: auto_camera, choosing the shots for a scene the way
    an editor would: on each line of dialogue (each time an actor starts a
    sound) the camera that shows the speaker best, facing us, at a good size
    and with their listeners in view; establishing shots that show everyone
    before anyone speaks. Cuts cost something, so the camera only changes
    when the new angle is clearly better, and shots too short to register
    are merged (directing.cpp holds those rules).

    The background's cameras are fixed angles, as in the Studio; the result
    is ordinary camera-change events, so the movie stays a normal .3mm.
***************************************************************************/
#include "bridge.h"
#include "directing.h"
#include "review.h"

#include <algorithm>
#include <cmath>

ASSERTNAME

namespace automation
{

// How a style weighs a shot: the speaker's framing against seeing the
// listeners, and what a cut costs
struct ShotStyle
{
    const char *pszName;
    double dFramingWeight;
    double dListenerWeight;
    double dCutPenalty;
    double dIdealHeight;  // speaker's on-screen height as a share of the frame
    double dCentreWeight; // how much keeping the speaker near the middle matters
    double dFaceFront;    // speaker facing us (up to a 3/4 view)
    double dFaceSide;     // in profile
    double dFaceBack;     // turned away
};
const ShotStyle krgstyle[] = {
    // establish wide, then singles on whoever speaks (shot / reverse shot)
    {"coverage", 2.5, 0.2, 0.4, 0.8, 1.0, 1.5, 0.6, -1.0},
    // keep everyone in frame, few cuts
    {"wide", 1.2, 1.0, 1.2, 0.5, 0.3, 0.8, 0.3, 0},
};
const double kdMinShotSecondsDefault = 2;
const double kdVisibleMinShot = 0.5; // less of the speaker showing is not a shot of them
const double kdFacingFront = 80;     // degrees from facing the camera that still show the face (3/4 view)
const double kdFacingSide = 110;     // profile
const double kdScoreMissing = -3;    // speaker not in shot

struct Beat
{
    int32_t nfrmStart;
    int32_t itrack; // speaker in the roll call, or -1 for an establishing beat
};

// Scores the current camera for a beat at the current frame
static double _DScoreShot(PMVIE pmvie, const std::vector<PACTR> &vecActors, int32_t iSpeaker, RC rcView,
                          const ShotStyle &style, bool *pfFaceAway)
{
    CameraModel camera;
    if (!FGetCamera(pmvie, &camera))
        return kdScoreMissing;
    EnsureRendered(pmvie);

    int32_t cOn = 0, cInShot = 0;
    double dHeightSum = 0, dScoreSpeaker = 0;
    for (size_t i = 0; i < vecActors.size(); i++)
    {
        PACTR pactr = vecActors[i];
        if (!pactr->FOnStage())
            continue;
        cOn++;
        RC rc;
        bool fInShot = pactr->FIsInView();
        if (fInShot)
        {
            pactr->GetRcBounds(&rc);
            ScreenBox box{rc.xpLeft, rc.ypTop, rc.xpRight, rc.ypBottom};
            fInShot = !FCutOffAtSides(box, rcView.Dxp()) && box.bottom > 0 && box.top < rcView.Dyp();
        }
        if (static_cast<int32_t>(i) == iSpeaker)
        {
            double dVisible = 0;
            if (!fInShot || !FVisibleFraction(pmvie, pactr, &dVisible) || dVisible < kdVisibleMinShot)
                return kdScoreMissing + dVisible;
            double degFacing = std::fabs(DegFacingFromCamera(pactr, camera));
            *pfFaceAway = degFacing > kdFacingSide;
            double dFace = degFacing <= kdFacingFront ? style.dFaceFront
                                                      : (degFacing <= kdFacingSide ? style.dFaceSide : style.dFaceBack);
            // 0 in the middle of the frame, 1 at either edge
            double dOffCentre = std::fabs((rc.xpLeft + rc.xpRight) / 2.0 - rcView.Dxp() / 2.0) / (rcView.Dxp() / 2.0);
            double dFraming = DFramingScore(static_cast<double>(rc.Dyp()) / rcView.Dyp(), style.dIdealHeight);
            dScoreSpeaker =
                2 + style.dCentreWeight * (1 - dOffCentre) + style.dFramingWeight * dFraming + dFace + dVisible;
            continue;
        }
        // Listeners only need to be in frame; an establishing shot must
        // really show everyone (not behind the furniture)
        double dVisible = 1;
        if (fInShot && iSpeaker < 0 && FVisibleFraction(pmvie, pactr, &dVisible) && dVisible < kdVisibleMinShot)
            fInShot = false;
        if (fInShot)
        {
            cInShot++;
            dHeightSum += static_cast<double>(rc.Dyp()) / rcView.Dyp();
        }
    }

    if (iSpeaker >= 0)
    {
        // Listeners in the shot help (the audience sees who is talked to)
        int32_t cListeners = cOn - 1;
        return dScoreSpeaker + (cListeners > 0 ? style.dListenerWeight * cInShot / cListeners : 0);
    }
    // Establishing: everyone in, at a readable size
    if (cOn == 0)
        return 0;
    return 2.5 * cInShot / cOn + (cInShot > 0 ? DFramingScore(dHeightSum / cInShot) : 0);
}

static bool _FCmdAutoCamera(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
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
    std::string strStyle = "coverage";
    if (!req.FArgString("style", &strStyle, fFalse))
        return fFalse;
    const ShotStyle *pstyle = nullptr;
    for (const ShotStyle &style : krgstyle)
    {
        if (StrLower(strStyle) == style.pszName)
            pstyle = &style;
    }
    if (pstyle == nullptr)
        return req.Fail(kszErrBadArgs, "'style' must be \"coverage\" or \"wide\"");
    double dMinShot = kdMinShotSecondsDefault, dCutPenalty = pstyle->dCutPenalty;
    bool fApply = fTrue, fVerbose = fFalse;
    if (!req.FArgBool("verbose", &fVerbose, fFalse) || !req.FArgNumber("min_shot_seconds", &dMinShot, fFalse) ||
        !req.FArgNumber("cut_penalty", &dCutPenalty, fFalse) || !req.FArgBool("apply", &fApply, fFalse))
    {
        return fFalse;
    }
    if (dMinShot < 0 || dMinShot > 30)
        return req.Fail(kszErrBadArgs, "'min_shot_seconds' must be between 0 and 30");
    if (dCutPenalty < 0 || dCutPenalty > 10)
        return req.Fail(kszErrBadArgs, "'cut_penalty' must be between 0 and 10");

    PBKGD pbkgd = pscen->Pbkgd();
    std::vector<int32_t> vecCams;
    const Json &jsonCams = req.args.Get("cameras");
    if (jsonCams.IsArray() && jsonCams.Size() > 0)
    {
        for (const Json &json : jsonCams.AsArray())
        {
            int32_t icam = json.IsNumber() ? static_cast<int32_t>(json.AsNumber()) : -1;
            if (icam < 0 || icam >= pbkgd->Ccam())
                return req.Fail(kszErrBadArgs, "camera indexes must be 0-" + std::to_string(pbkgd->Ccam() - 1));
            vecCams.push_back(icam);
        }
    }
    else
    {
        for (int32_t icam = 0; icam < pbkgd->Ccam(); icam++)
            vecCams.push_back(icam);
    }

    int32_t nfrmFirst = pscen->NfrmFirst(), nfrmLast = pscen->NfrmLast();
    int32_t nfrmRestore = pscen->Nfrm();
    std::vector<PACTR> vecActors;
    PGL pglpactr = pscen->PglRollCall();
    for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
    {
        PACTR pactr;
        pglpactr->Get(ipactr, &pactr);
        vecActors.push_back(pactr);
    }

    // Beats: an establishing beat until the first line, then one per line
    std::vector<Beat> vecBeats;
    for (int32_t nfrm = nfrmFirst; nfrm <= nfrmLast; nfrm++)
    {
        if (!pscen->FGotoFrm(nfrm))
            return FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
        for (size_t i = 0; i < vecActors.size(); i++)
        {
            if (vecActors[i]->FOnStage() && vecActors[i]->FSoundInFrm())
            {
                if (!vecBeats.empty() && vecBeats.back().nfrmStart == nfrm)
                    continue; // two sounds at once: the first actor leads
                vecBeats.push_back({nfrm, static_cast<int32_t>(i)});
            }
        }
    }
    if (vecBeats.empty() || vecBeats.front().nfrmStart > nfrmFirst)
        vecBeats.insert(vecBeats.begin(), {nfrmFirst, -1});

    // Score every camera on every beat: the worst of frames near its start,
    // middle and end, so a walk out of shot or behind the furniture counts
    RC rcView(0, 0, 544, 306);
    if (PmvuCur() != pvNil)
        PmvuCur()->GetRc(&rcView, cooLocal);
    int32_t icamRestore = pbkgd->Icam();
    std::vector<std::vector<double>> vecvecScore(vecBeats.size(), std::vector<double>(vecCams.size(), 1e9));
    std::vector<std::vector<bool>> vecvecFaceAway(vecBeats.size(), std::vector<bool>(vecCams.size(), false));
    for (size_t ibeat = 0; ibeat < vecBeats.size(); ibeat++)
    {
        int32_t nfrmLim = ibeat + 1 < vecBeats.size() ? vecBeats[ibeat + 1].nfrmStart : nfrmLast + 1;
        int32_t nfrmStart = vecBeats[ibeat].nfrmStart;
        for (int32_t nfrm :
             {LwMin(nfrmStart + 1, nfrmLim - 1), nfrmStart + (nfrmLim - nfrmStart) / 2, LwMax(nfrmStart, nfrmLim - 2)})
        {
            if (!pscen->FGotoFrm(nfrm))
                return FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
            for (size_t icam = 0; icam < vecCams.size(); icam++)
            {
                if (!pbkgd->FSetCamera(pmvie->Pbwld(), vecCams[icam]))
                    return FFailEngine(req, "could not try camera " + std::to_string(vecCams[icam]));
                bool fFaceAway = false;
                double d = _DScoreShot(pmvie, vecActors, vecBeats[ibeat].itrack, rcView, *pstyle, &fFaceAway);
                if (fFaceAway)
                    vecvecFaceAway[ibeat][icam] = true;
                vecvecScore[ibeat][icam] = std::min(vecvecScore[ibeat][icam], d);
            }
        }
    }
    pbkgd->FSetCamera(pmvie->Pbwld(), icamRestore);

    std::vector<int32_t> vecChoice = VecChooseShots(vecvecScore, dCutPenalty);
    std::vector<int32_t> vecBeatStart;
    for (const Beat &beat : vecBeats)
        vecBeatStart.push_back(beat.nfrmStart);
    HoldShortShots(vecBeatStart, nfrmLast + 1, static_cast<int32_t>(dMinShot * kfps + 0.5), &vecChoice);

    // Apply: walk the scene and change the camera wherever it differs (a
    // change back to the previous camera removes an old cut)
    int32_t ccut = 0;
    if (fApply)
    {
        size_t ibeat = 0;
        for (int32_t nfrm = nfrmFirst; nfrm <= nfrmLast; nfrm++)
        {
            while (ibeat + 1 < vecBeats.size() && vecBeats[ibeat + 1].nfrmStart <= nfrm)
                ibeat++;
            if (!pscen->FGotoFrm(nfrm))
                return FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
            int32_t icam = vecCams[vecChoice[ibeat]];
            if (pscen->Pbkgd()->Icam() != icam && !pmvie->FChangeCam(icam))
                return FFailEngine(req, "could not change camera at frame " + std::to_string(nfrm));
        }
        pmvie->SetDirty();
    }

    // The shot list
    Json jsonShots = Json::MakeArray();
    for (size_t ibeat = 0; ibeat < vecBeats.size(); ibeat++)
    {
        if (ibeat > 0 && vecChoice[ibeat] == vecChoice[ibeat - 1])
            continue;
        size_t ibeatLim = ibeat + 1;
        while (ibeatLim < vecBeats.size() && vecChoice[ibeatLim] == vecChoice[ibeat])
            ibeatLim++;
        Json json = Json::MakeObject();
        json.Set("from", vecBeats[ibeat].nfrmStart);
        json.Set("to", ibeatLim < vecBeats.size() ? vecBeats[ibeatLim].nfrmStart - 1 : nfrmLast);
        json.Set("camera", vecCams[vecChoice[ibeat]]);
        int32_t itrack = vecBeats[ibeat].itrack;
        STN stn;
        json.Set("on", itrack < 0 ? std::string("everyone (establishing)")
                                  : (pmvie->FGetName(vecActors[itrack]->Arid(), &stn)
                                         ? StrFromStn(&stn)
                                         : "actor " + std::to_string(vecActors[itrack]->Arid())));
        if (vecvecScore[ibeat][vecChoice[ibeat]] < 0)
            json.Set("warning", "no camera shows this speaker well; move them (arrange_actors) or accept it");
        else if (std::any_of(vecvecFaceAway.begin() + ibeat, vecvecFaceAway.begin() + ibeatLim,
                             [&](const std::vector<bool> &vec) { return static_cast<bool>(vec[vecChoice[ibeat]]); }))
        {
            json.Set("warning", "the speaker has their back to this camera and no angle shows their face; turn "
                                "them toward the camera a little (rotate_actor) or re-block (arrange_actors), then "
                                "run auto_camera again");
        }
        jsonShots.Push(json);
        ccut++;
    }
    pscen->FGotoFrm(nfrmRestore);
    RefreshViews(pmvie);
    req.result.Set("style", pstyle->pszName);
    req.result.Set("applied", fApply);
    req.result.Set("beats", static_cast<int>(vecBeats.size()));
    req.result.Set("shots", jsonShots);
    req.result.Set("cuts", LwMax(0, ccut - 1));
    if (fVerbose)
    {
        // Each beat's score for each camera, for judging the choice
        Json jsonBeats = Json::MakeArray();
        for (size_t ibeat = 0; ibeat < vecBeats.size(); ibeat++)
        {
            Json json = Json::MakeObject();
            json.Set("frame", vecBeats[ibeat].nfrmStart);
            Json jsonScores = Json::MakeObject();
            for (size_t icam = 0; icam < vecCams.size(); icam++)
                jsonScores.Set(std::to_string(vecCams[icam]), std::round(vecvecScore[ibeat][icam] * 100) / 100);
            json.Set("scores", jsonScores);
            jsonBeats.Push(json);
        }
        req.result.Set("beat_scores", jsonBeats);
    }
    if (fApply)
        req.result.Set("note", "undo only reverts the last cut; checkpoint first if you may want the old shots back");
    return fTrue;
}

const CommandInfo krgcmdDirect[] = {
    {"auto_camera", _FCmdAutoCamera, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
