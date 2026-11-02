/***************************************************************************
    Automation bridge: review_movie, an automatic check of the movie for
    problems a viewer would notice: actors out of shot, cut off by the frame
    or hidden behind someone nearer the camera (worst while they speak),
    captions too short to read, long stretches where nothing changes, and
    empty or very short scenes. Each problem names its scene, frames and a
    fix, so an AI can check its work without looking at every frame.

    Every frame is stepped (cheap) to track movement, actions, sounds,
    cameras and text; the picture is rendered every half second and at each
    cut and line of dialogue to see who is in shot.
***************************************************************************/
#include "bridge.h"
#include "review.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

ASSERTNAME

namespace automation
{

const int32_t kcfrmSampleStep = 3; // render every half second
const int32_t kcfrmSpeech = 12;    // a line counts as being spoken for two seconds after it starts
const double kdSecStaticMax = 8;   // flag longer stretches where nothing changes
const double kdSecSceneMin = 1.5;  // shorter scenes flash by
const double kdVisibleMin = 0.4;   // less of its silhouette showing counts as hidden
const double kdOccluderMin = 0.3;
const double kdOverlapShare = 0.7; // closer than this share of their radii counts as inside each other  // box overlap
                                   // that names a nearer actor as the one in the way
const int32_t kcTinyDivisor = 6;   // shorter than this fraction of the view is hard to see
const double kdMoveMin = 0.02;     // metres per frame; less is standing still
const int32_t kcissueMax = 100;    // keep the reply readable

struct Issue
{
    const char *pszSeverity; // "warning": a viewer would notice; "info": worth a look
    const char *pszKind;
    int32_t iscen;
    FrameRange range;
    std::string strActor;
    std::string strMessage;
    std::string strFix;
};

struct ActorTrack
{
    PACTR pactr;
    std::string strName;
    std::set<int32_t> setMoving;          // frames where it moved since the previous frame
    std::vector<int32_t> vecSpeechStarts; // frames where its sounds start
    int32_t csampleOnStage = 0;
    std::vector<int32_t> vecSeen, vecOut, vecCutOff, vecHidden, vecTiny; // sampled frames
    std::map<int32_t, std::string> mapHiddenBy;                          // what is in the way at each hidden sample
};

static std::string _StrSeconds(double dSec)
{
    char sz[32];
    snprintf(sz, sizeof(sz), "%.1f s", dSec);
    return sz;
}

static std::string _StrActorName(PMVIE pmvie, PACTR pactr)
{
    STN stn;
    if (pmvie->FGetName(pactr->Arid(), &stn) && stn.Cch() > 0)
        return StrFromStn(&stn);
    pactr->Ptmpl()->GetName(&stn);
    return StrFromStn(&stn) + " (id " + std::to_string(pactr->Arid()) + ")";
}

static bool _FSpeaking(const ActorTrack &track, const FrameRange &range)
{
    for (int32_t nfrm : track.vecSpeechStarts)
    {
        if (nfrm <= range.to && nfrm + kcfrmSpeech > range.from)
            return true;
    }
    return false;
}

// Whether another actor speaks during range: then this one being out of
// frame is ordinary cutting to the speaker, not a mistake
static bool _FOtherSpeaking(const std::vector<ActorTrack> &vecTracks, const ActorTrack &track, const FrameRange &range)
{
    for (const ActorTrack &other : vecTracks)
    {
        if (&other != &track && _FSpeaking(other, range))
            return true;
    }
    return false;
}

static bool _FMoving(const ActorTrack &track, const FrameRange &range)
{
    auto it = track.setMoving.lower_bound(range.from - kcfrmSampleStep);
    return it != track.setMoving.end() && *it <= range.to + kcfrmSampleStep;
}

// Whether an action is one an actor holds while idle (doesn't count as
// something happening)
static bool _FRestAction(PACTR pactr, int32_t anid)
{
    return StrLower(StrActionName(pactr->Ptmpl(), anid)).find("rest") != std::string::npos;
}

/***************************************************************************
    Reviews one scene, appending to vecIssues. Leaves the scene at an
    arbitrary frame.
***************************************************************************/
static bool _FReviewScene(Request &req, PMVIE pmvie, int32_t iscen, std::vector<Issue> *pvecIssues, int32_t *pccut)
{
    PSCEN pscen = pmvie->Pscen();
    int32_t nfrmFirst = pscen->NfrmFirst(), nfrmLast = pscen->NfrmLast();
    int32_t cfrm = nfrmLast - nfrmFirst + 1;
    auto add = [&](const char *pszSeverity, const char *pszKind, FrameRange range, const std::string &strActor,
                   const std::string &strMessage, const std::string &strFix) {
        pvecIssues->push_back({pszSeverity, pszKind, iscen, range, strActor, strMessage, strFix});
    };

    PGL pglpactr = pscen->PglRollCall();
    std::vector<ActorTrack> vecTracks;
    for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
    {
        ActorTrack track;
        pglpactr->Get(ipactr, &track.pactr);
        track.strName = _StrActorName(pmvie, track.pactr);
        vecTracks.push_back(track);
    }

    if (cfrm / static_cast<double>(kfps) < kdSecSceneMin)
    {
        add("info", "short_scene", {nfrmFirst, nfrmLast}, "",
            "the scene lasts only " + _StrSeconds(cfrm / static_cast<double>(kfps)),
            "make it longer (animate, add_pause) or remove it (delete_scene) if it isn't meant as a quick cut");
    }
    if (vecTracks.empty() && pscen->Ctbox() == 0)
    {
        add("warning", "empty_scene", {nfrmFirst, nfrmLast}, "", "the scene has no actors and no text",
            "add actors (add_actor) or a title (add_text), or delete_scene");
    }

    // Pass 1: step every frame without rendering
    std::set<int32_t> setSample;
    std::vector<bool> vecActive(cfrm, false);
    std::vector<std::vector<bool>> vecvecTboxVisible(pscen->Ctbox(), std::vector<bool>(cfrm, false));
    std::vector<Vec3> vecPrevPos(vecTracks.size());
    std::vector<int32_t> vecPrevAnid(vecTracks.size(), ivNil);
    std::vector<bool> vecPrevOnStage(vecTracks.size(), false);
    int32_t icamPrev = ivNil;
    for (int32_t nfrm = nfrmFirst; nfrm <= nfrmLast; nfrm++)
    {
        int32_t ifrm = nfrm - nfrmFirst;
        if (!pscen->FGotoFrm(nfrm))
            return FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
        if ((nfrm - nfrmFirst) % kcfrmSampleStep == 0 || nfrm == nfrmLast)
            setSample.insert(nfrm);

        bool fActive = false;
        int32_t icam = pscen->Pbkgd()->Icam();
        if (icam != icamPrev)
        {
            if (icamPrev != ivNil)
                (*pccut)++;
            setSample.insert(nfrm);
            fActive = true;
            icamPrev = icam;
        }
        for (size_t itrack = 0; itrack < vecTracks.size(); itrack++)
        {
            ActorTrack &track = vecTracks[itrack];
            PACTR pactr = track.pactr;
            BRS xr, yr, zr;
            pactr->Pbody()->GetPosition(&xr, &yr, &zr);
            Vec3 pos(DFromBrs(xr), DFromBrs(yr), DFromBrs(zr));
            bool fOnStage = pactr->FOnStage();
            if (fOnStage && vecPrevOnStage[itrack] && (pos - vecPrevPos[itrack]).Length() > kdMoveMin * kdwrPerMetre)
            {
                track.setMoving.insert(nfrm);
                fActive = true;
            }
            if (fOnStage != vecPrevOnStage[itrack] || (fOnStage && pactr->AnidCur() != vecPrevAnid[itrack]))
                fActive = true;
            int32_t ccel = 0;
            if (fOnStage && !pactr->FFrozen() && pactr->Ptmpl()->FGetCcelActn(pactr->AnidCur(), &ccel) && ccel > 1 &&
                !_FRestAction(pactr, pactr->AnidCur()))
            {
                fActive = true; // acting (talking, dancing...) on the spot
            }
            if (pactr->FSoundInFrm())
            {
                track.vecSpeechStarts.push_back(nfrm);
                setSample.insert(nfrm);
                fActive = true;
            }
            vecPrevPos[itrack] = pos;
            vecPrevAnid[itrack] = pactr->AnidCur();
            vecPrevOnStage[itrack] = fOnStage;
        }
        for (int32_t itbox = 0; itbox < pscen->Ctbox(); itbox++)
        {
            PTBOX ptbox = pscen->PtboxFromItbox(itbox);
            vecvecTboxVisible[itbox][ifrm] = ptbox != pvNil && ptbox->FIsVisible();
            if (ifrm > 0 && vecvecTboxVisible[itbox][ifrm] != vecvecTboxVisible[itbox][ifrm - 1])
                fActive = true;
        }
        for (int32_t sty = stySfx; sty < styLim && !fActive; sty++)
        {
            PGL pgltag = pvNil;
            int32_t vlm;
            bool fLoop;
            if (pscen->FQuerySnd(sty, &pgltag, &vlm, &fLoop) && pgltag != pvNil && pgltag->IvMac() > 0)
                fActive = true;
            ReleasePpo(&pgltag);
        }
        vecActive[ifrm] = fActive;
    }

    // Pass 2: render the sampled frames and see who is in shot
    RC rcView(0, 0, 544, 306);
    if (PmvuCur() != pvNil)
        PmvuCur()->GetRc(&rcView, cooLocal);
    std::map<std::pair<size_t, size_t>, std::vector<int32_t>> mapCollisions; // sampled frames per pair
    for (int32_t nfrm : setSample)
    {
        if (!pscen->FGotoFrm(nfrm))
            return FFailEngine(req, "could not go to frame " + std::to_string(nfrm));
        CameraModel camera;
        if (!FGetCamera(pmvie, &camera))
            return FFailEngine(req, "no camera");
        EnsureRendered(pmvie);

        struct InShot
        {
            size_t itrack;
            ScreenBox box;
            double dDistance;
        };
        std::vector<InShot> vecInShot;
        for (size_t itrack = 0; itrack < vecTracks.size(); itrack++)
        {
            ActorTrack &track = vecTracks[itrack];
            PACTR pactr = track.pactr;
            if (!pactr->FOnStage())
                continue;
            track.csampleOnStage++;
            if (!pactr->FIsInView())
            {
                track.vecOut.push_back(nfrm);
                continue;
            }
            RC rc;
            pactr->GetRcBounds(&rc);
            ScreenBox box{rc.xpLeft, rc.ypTop, rc.xpRight, rc.ypBottom};
            if (box.right <= 0 || box.left >= rcView.Dxp() || box.bottom <= 0 || box.top >= rcView.Dyp())
            {
                track.vecOut.push_back(nfrm);
                continue;
            }
            track.vecSeen.push_back(nfrm);
            if (FCutOffAtSides(box, rcView.Dxp()))
                track.vecCutOff.push_back(nfrm);
            else if (rc.Dyp() < rcView.Dyp() / kcTinyDivisor)
                track.vecTiny.push_back(nfrm);
            BRS xr, yr, zr;
            pactr->Pbody()->GetPosition(&xr, &yr, &zr);
            Vec3 pos(DFromBrs(xr), DFromBrs(yr), DFromBrs(zr));
            vecInShot.push_back({itrack, box, (pos - camera.position).Length()});
        }
        // Hidden: most of its silhouette is covered, by scenery or a nearer
        // actor. Bounding boxes alone overstate it: a half-covered actor
        // with a clear face is fine.
        for (const InShot &back : vecInShot)
        {
            double dVisible;
            if (!FVisibleFraction(pmvie, vecTracks[back.itrack].pactr, &dVisible) || dVisible >= kdVisibleMin)
                continue;
            std::string strBy = "the scenery";
            for (const InShot &front : vecInShot)
            {
                if (front.dDistance < back.dDistance && DCoveredFraction(back.box, front.box) >= kdOccluderMin)
                {
                    strBy = vecTracks[front.itrack].strName;
                    break;
                }
            }
            vecTracks[back.itrack].vecHidden.push_back(nfrm);
            vecTracks[back.itrack].mapHiddenBy[nfrm] = strBy;
        }

        // Collisions: two actors' bodies inside each other
        std::vector<size_t> vecOn;
        std::vector<Vec3> vecPos;
        std::vector<double> vecRadius, vecHeight;
        for (size_t itrack = 0; itrack < vecTracks.size(); itrack++)
        {
            PACTR pactr = vecTracks[itrack].pactr;
            if (!pactr->FOnStage())
                continue;
            BRS xr, yr, zr;
            double dRadius, dHeight;
            pactr->Pbody()->GetPosition(&xr, &yr, &zr);
            GetActorFootprint(pactr, &dRadius, &dHeight);
            vecOn.push_back(itrack);
            vecPos.push_back(Vec3(DFromBrs(xr), DFromBrs(yr), DFromBrs(zr)) * (1 / kdwrPerMetre));
            vecRadius.push_back(dRadius);
            vecHeight.push_back(dHeight);
        }
        for (size_t i = 0; i < vecOn.size(); i++)
        {
            for (size_t j = i + 1; j < vecOn.size(); j++)
            {
                double dist = std::hypot(vecPos[i].x - vecPos[j].x, vecPos[i].z - vecPos[j].z);
                bool fLevel = vecPos[i].y < vecPos[j].y + vecHeight[j] && vecPos[j].y < vecPos[i].y + vecHeight[i];
                if (fLevel && dist < kdOverlapShare * (vecRadius[i] + vecRadius[j]))
                    mapCollisions[{vecOn[i], vecOn[j]}].push_back(nfrm);
            }
        }
    }

    const std::string kstrFixShot = "move them into view (render_frame(overlay: true) shows the floor grid the camera "
                                    "sees) or pick a camera that frames everyone (suggest_camera, set_camera)";
    for (const ActorTrack &track : vecTracks)
    {
        if (track.csampleOnStage == 0)
            continue;
        if (track.vecSeen.empty())
        {
            add("warning", "never_in_shot", {nfrmFirst, nfrmLast}, track.strName,
                track.strName + " is on stage but never in shot", kstrFixShot);
            continue;
        }
        for (const FrameRange &range : VecRanges(track.vecOut, kcfrmSampleStep))
        {
            bool fSpeaking = _FSpeaking(track, range), fMoving = _FMoving(track, range);
            bool fListening = !fSpeaking && _FOtherSpeaking(vecTracks, track, range);
            add(fSpeaking || (!fMoving && !fListening) ? "warning" : "info", "out_of_shot", range, track.strName,
                track.strName + " is out of shot" +
                    (fSpeaking ? " while speaking"
                               : (fMoving ? " (walking in or out)" : (fListening ? " while someone else speaks" : ""))),
                kstrFixShot);
        }
        for (const FrameRange &range : VecRanges(track.vecCutOff, kcfrmSampleStep))
        {
            bool fSpeaking = _FSpeaking(track, range), fMoving = _FMoving(track, range);
            add(fSpeaking ? "warning" : "info", "cut_off", range, track.strName,
                track.strName + " is cut off by the edge of the frame" +
                    (fSpeaking ? " while speaking" : (fMoving ? " (walking in or out)" : "")),
                "move them towards the middle (x nearer 0) or further from the camera (higher z), or use a wider "
                "camera");
        }
        for (const FrameRange &range : VecRanges(track.vecHidden, kcfrmSampleStep))
        {
            bool fSpeaking = _FSpeaking(track, range);
            bool fListening = !fSpeaking && _FOtherSpeaking(vecTracks, track, range);
            // Passing behind something on the way in or out is staging, not a mistake
            bool fPassing = !fSpeaking && _FMoving(track, range);
            // Someone walking across in front for a moment is staging too
            const std::string &strBy = track.mapHiddenBy.at(range.from);
            auto itBy = std::find_if(vecTracks.begin(), vecTracks.end(),
                                     [&](const ActorTrack &other) { return other.strName == strBy; });
            bool fCrossedBy = itBy != vecTracks.end() && _FMoving(*itBy, range) && range.to - range.from < kfps;
            add(fListening || fPassing || fCrossedBy ? "info" : "warning", "hidden", range, track.strName,
                track.strName + " is mostly hidden behind " + strBy +
                    (fSpeaking    ? " while speaking"
                     : fListening ? " while someone else speaks"
                     : fPassing   ? " while walking"
                     : fCrossedBy ? " as they walk past"
                                  : ""),
                "move them (arrange_actors does a group), spread the actors out sideways (x) or pick a camera "
                "from another side");
        }
        for (const FrameRange &range : VecRanges(track.vecTiny, kcfrmSampleStep))
        {
            add("info", "tiny", range, track.strName, track.strName + " looks very small (far from the camera)",
                "move them closer to the camera (lower z) or use a closer camera");
        }
    }

    for (const auto &entry : mapCollisions)
    {
        const ActorTrack &trackA = vecTracks[entry.first.first], &trackB = vecTracks[entry.first.second];
        for (const FrameRange &range : VecRanges(entry.second, kcfrmSampleStep))
        {
            bool fMoving = _FMoving(trackA, range) || _FMoving(trackB, range);
            add("warning", "collision", range, trackA.strName,
                trackA.strName + " and " + trackB.strName +
                    (fMoving ? " walk through each other" : " stand inside each other"),
                "move one of them (place_actor, arrange_actors), route the walk around the other (move_actor "
                "with avoid_actors), or have them bump with physics");
        }
    }

    // Long stretches where nothing changes
    std::vector<int32_t> vecQuiet;
    for (int32_t ifrm = 0; ifrm < cfrm; ifrm++)
    {
        if (!vecActive[ifrm])
            vecQuiet.push_back(nfrmFirst + ifrm);
    }
    for (const FrameRange &range : VecRanges(vecQuiet, 1))
    {
        double dSec = (range.to - range.from + 1) / static_cast<double>(kfps);
        if (dSec > kdSecStaticMax)
        {
            add("info", "nothing_happens", range, "",
                "nothing changes for " + _StrSeconds(dSec) + " (no movement, new action, cut, sound or text)",
                "add a reaction (animate), a camera cut (set_camera) or shorten the scene (trim_scene)");
        }
    }

    // Text that isn't up long enough to read
    for (int32_t itbox = 0; itbox < pscen->Ctbox(); itbox++)
    {
        PTBOX ptbox = pscen->PtboxFromItbox(itbox);
        if (ptbox == pvNil)
            continue;
        std::vector<int32_t> vecVisible;
        for (int32_t ifrm = 0; ifrm < cfrm; ifrm++)
        {
            if (vecvecTboxVisible[itbox][ifrm])
                vecVisible.push_back(nfrmFirst + ifrm);
        }
        std::string strText = StrTboxText(ptbox);
        double dSecNeeded = DSecondsToRead(strText);
        std::string strShort = strText.size() > 40 ? strText.substr(0, 37) + "..." : strText;
        for (const FrameRange &range : VecRanges(vecVisible, 1))
        {
            double dSec = (range.to - range.from + 1) / static_cast<double>(kfps);
            if (dSec + 0.01 < dSecNeeded)
            {
                add("warning", "text_too_short", range, "",
                    "text " + std::to_string(itbox) + " (\"" + strShort + "\") is on screen for " + _StrSeconds(dSec) +
                        "; reading it takes about " + _StrSeconds(dSecNeeded),
                    "remove_text and add_text again with a later until_frame, or shorten the text");
            }
        }
    }
    return fTrue;
}

static bool _FCmdReviewMovie(Request &req)
{
    PMVIE pmvie;
    if (!FRequireMovie(req, &pmvie) || !FRequireNotPlaying(req, pmvie))
        return fFalse;
    if (pmvie->Cscen() == 0)
        return req.Fail(kszErrBadState, "the movie has no scenes");

    int32_t iscenFirst = 0, iscenLim = pmvie->Cscen();
    if (req.FHasArg("scene"))
    {
        int32_t iscen;
        if (!req.FArgInt("scene", &iscen))
            return fFalse;
        if (iscen < 0 || iscen >= pmvie->Cscen())
            return req.Fail(kszErrBadArgs, "scene " + std::to_string(iscen) + " does not exist");
        iscenFirst = iscen;
        iscenLim = iscen + 1;
    }

    int32_t iscenRestore = pmvie->Iscen();
    int32_t nfrmRestore = pmvie->Pscen()->Nfrm();
    std::vector<Issue> vecIssues;
    int32_t cfrmTotal = 0, ccut = 0, cshot = 0;
    bool fSound = false;
    for (int32_t iscen = iscenFirst; iscen < iscenLim; iscen++)
    {
        if (iscen != pmvie->Iscen() && !pmvie->FSwitchScen(iscen))
            return FFailEngine(req, "could not switch to scene " + std::to_string(iscen));
        if (!_FReviewScene(req, pmvie, iscen, &vecIssues, &ccut))
            return fFalse;
        PSCEN pscen = pmvie->Pscen();
        cfrmTotal += pscen->NfrmLast() - pscen->NfrmFirst() + 1;
        cshot++;
        // Any sound at all? (scene sounds or actor sounds anywhere)
        for (int32_t nfrm = pscen->NfrmFirst(); nfrm <= pscen->NfrmLast() && !fSound; nfrm++)
        {
            if (!pscen->FGotoFrm(nfrm))
                break;
            PGL pglpactr = pscen->PglRollCall();
            for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac() && !fSound; ipactr++)
            {
                PACTR pactr;
                pglpactr->Get(ipactr, &pactr);
                fSound = pactr->FSoundInFrm();
            }
            for (int32_t sty = stySfx; sty < styLim && !fSound; sty++)
            {
                PGL pgltag = pvNil;
                int32_t vlm;
                bool fLoop;
                fSound = pscen->FQuerySnd(sty, &pgltag, &vlm, &fLoop) && pgltag != pvNil && pgltag->IvMac() > 0;
                ReleasePpo(&pgltag);
            }
        }
    }
    cshot += ccut;
    if (!fSound)
    {
        vecIssues.push_back({"info",
                             "silent",
                             iscenFirst,
                             {ivNil, ivNil},
                             "",
                             "there is no sound at all",
                             "give characters lines (speak_line) or add music (attach_sound)"});
    }

    if (iscenRestore != pmvie->Iscen())
        pmvie->FSwitchScen(iscenRestore);
    pmvie->Pscen()->FGotoFrm(nfrmRestore);
    RefreshViews(pmvie);

    // Warnings first, then by scene and frame
    std::stable_sort(vecIssues.begin(), vecIssues.end(), [](const Issue &a, const Issue &b) {
        bool fWarnA = std::string(a.pszSeverity) == "warning", fWarnB = std::string(b.pszSeverity) == "warning";
        if (fWarnA != fWarnB)
            return fWarnA;
        if (a.iscen != b.iscen)
            return a.iscen < b.iscen;
        return a.range.from < b.range.from;
    });

    int32_t cwarning = 0;
    Json jsonIssues = Json::MakeArray();
    for (const Issue &issue : vecIssues)
    {
        bool fWarning = std::string(issue.pszSeverity) == "warning";
        cwarning += fWarning;
        if (static_cast<int32_t>(jsonIssues.Size()) >= kcissueMax)
            continue;
        Json json = Json::MakeObject();
        json.Set("severity", issue.pszSeverity);
        json.Set("kind", issue.pszKind);
        json.Set("scene", issue.iscen);
        if (issue.range.from != ivNil)
        {
            json.Set("from_frame", issue.range.from);
            json.Set("to_frame", issue.range.to);
        }
        if (!issue.strActor.empty())
            json.Set("actor", issue.strActor);
        json.Set("message", issue.strMessage);
        json.Set("fix", issue.strFix);
        jsonIssues.Push(json);
    }

    Json jsonSummary = Json::MakeObject();
    jsonSummary.Set("scenes", iscenLim - iscenFirst);
    jsonSummary.Set("seconds", std::round(cfrmTotal * 10.0 / kfps) / 10);
    jsonSummary.Set("shots", cshot);
    jsonSummary.Set("average_shot_seconds", std::round(cfrmTotal * 10.0 / kfps / LwMax(1, cshot)) / 10);
    jsonSummary.Set("warnings", cwarning);
    jsonSummary.Set("notes", static_cast<int>(vecIssues.size()) - cwarning);
    req.result.Set("summary", jsonSummary);
    req.result.Set("issues", jsonIssues);
    if (static_cast<int32_t>(vecIssues.size()) > kcissueMax)
        req.result.Set("truncated", static_cast<int>(vecIssues.size()) - kcissueMax);
    req.result.Set("verdict", cwarning == 0 ? "no problems a viewer would notice"
                                            : std::to_string(cwarning) + " problem(s) a viewer would notice");
    return fTrue;
}

const CommandInfo krgcmdReview[] = {
    {"review_movie", _FCmdReviewMovie, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
