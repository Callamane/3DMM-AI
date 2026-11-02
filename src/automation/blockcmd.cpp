/***************************************************************************
    Automation bridge: arrange_actors, blocking a group of actors in one
    call. A conversation stands the actors on a shallow arc, each turned
    toward a point just in front of the group, so partners face each other
    while staying open to the camera; a line stands them in a row facing the
    camera. Unless told the depth, the group starts close to the camera and
    steps back until everyone is fully in shot and nobody hides anybody.
***************************************************************************/
#include "bridge.h"
#include "directing.h"
#include "review.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

ASSERTNAME

namespace automation
{

const double kdSpacingDefault = 1.0;    // metres between neighbours
const double kdHiddenMaxArrange = 0.35; // box overlap allowed with a nearer actor: blocking keeps faces clear
const double kdVisibleMinArrange = 0.8; // share of each silhouette that must show
const double kdNearFloorRow = 0.95;     // share of the view height where "the bottom of the picture" is measured
const double krgdShift[] = {0, 0.6, -0.6, 1.2, -1.2}; // sideways tries, metres
// Metres beyond the nearest floor the camera sees (where the bottom of the
// picture meets the floor), so the search starts in front of the camera
// whatever the set's placement point is
const double krgdDepth[] = {1.0, 1.4, 1.8, 2.3, 2.8, 3.4, 4.1, 5.0};
// Coverage: other cameras that show someone's face give the editor angles
// to cut to. Spots up to this many depth steps behind the first good one
// compete on it (no further: the main shot would get too small)
const int32_t kcDepthCoverage = 1;
const double kdFacingCover = 80;      // degrees from the camera that still show the face (3/4 view)
const double kdHeightCoverMin = 0.3;  // share of the view height a face needs to read
const double kdVisibleCoverMin = 0.5; // share of the silhouette showing

// Which actors each other camera shows face-on at a readable size, from
// the group's current spot and the main camera's side of each actor's line
// with their nearest partner (a cut across it swaps them on screen);
// leaves the scene on icamMain
static std::vector<std::vector<bool>> _VecCoverage(PMVIE pmvie, const std::vector<PACTR> &vecActors, RC rcView,
                                                   int32_t icamMain)
{
    PBKGD pbkgd = pmvie->Pscen()->Pbkgd();
    std::vector<std::vector<bool>> vecvecShows(pbkgd->Ccam(), std::vector<bool>(vecActors.size(), false));
    CameraModel cameraMain;
    if (!FGetCamera(pmvie, &cameraMain))
        return vecvecShows;
    std::vector<Vec3> vecPos;
    for (PACTR pactr : vecActors)
    {
        BRS xr, yr, zr;
        pactr->Pbody()->GetPosition(&xr, &yr, &zr);
        vecPos.push_back(Vec3(DFromBrs(xr), DFromBrs(yr), DFromBrs(zr)));
    }
    auto sameSide = [&](size_t i, const CameraModel &camera) {
        size_t iNearest = i;
        for (size_t j = 0; j < vecPos.size(); j++)
        {
            if (j != i && (iNearest == i || (vecPos[j] - vecPos[i]).Length() < (vecPos[iNearest] - vecPos[i]).Length()))
                iNearest = j;
        }
        return iNearest == i ||
               FSameSideOfLine(vecPos[i].x, vecPos[i].z, vecPos[iNearest].x, vecPos[iNearest].z, cameraMain.position.x,
                               cameraMain.position.z, camera.position.x, camera.position.z);
    };
    for (int32_t icam = 0; icam < pbkgd->Ccam(); icam++)
    {
        CameraModel camera;
        if (icam == icamMain || !pbkgd->FSetCamera(pmvie->Pbwld(), icam) || !FGetCamera(pmvie, &camera))
            continue;
        EnsureRendered(pmvie);
        for (size_t i = 0; i < vecActors.size(); i++)
        {
            PACTR pactr = vecActors[i];
            RC rc;
            double dVisible;
            if (!pactr->FIsInView())
                continue;
            pactr->GetRcBounds(&rc);
            ScreenBox box{rc.xpLeft, rc.ypTop, rc.xpRight, rc.ypBottom};
            vecvecShows[icam][i] = sameSide(i, camera) && !FCutOffAtSides(box, rcView.Dxp()) &&
                                   static_cast<double>(rc.Dyp()) / rcView.Dyp() >= kdHeightCoverMin &&
                                   std::fabs(DegFacingFromCamera(pactr, camera)) <= kdFacingCover &&
                                   FVisibleFraction(pmvie, pactr, &dVisible) && dVisible >= kdVisibleCoverMin;
        }
    }
    pbkgd->FSetCamera(pmvie->Pbwld(), icamMain);
    EnsureRendered(pmvie);
    return vecvecShows;
}

// Higher is better: first how many actors have another angle on their
// face, then how many such angles there are in all
static int32_t _CCoverageScore(const std::vector<std::vector<bool>> &vecvecShows)
{
    if (vecvecShows.empty())
        return 0;
    int32_t cActorsCovered = 0, cAngles = 0;
    for (size_t i = 0; i < vecvecShows[0].size(); i++)
    {
        bool fCovered = false;
        for (const std::vector<bool> &vecShows : vecvecShows)
        {
            fCovered = fCovered || vecShows[i];
            cAngles += vecShows[i] ? 1 : 0;
        }
        cActorsCovered += fCovered ? 1 : 0;
    }
    return cActorsCovered * 100 + cAngles;
}

static std::string _StrMetres(double d)
{
    char sz[32];
    snprintf(sz, sizeof(sz), "%.1f", d);
    return sz;
}

static bool _FCmdArrangeActors(Request &req)
{
    PMVIE pmvie;
    PSCEN pscen;
    if (!FRequireScene(req, &pmvie, &pscen) || !FRequireNotPlaying(req, pmvie) || !FApplyFrameArg(req, pscen))
        return fFalse;

    // Who: the listed actors, or everyone on stage
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
            if (!pactr->FOnStage())
                return req.Fail(kszErrBadState, "an actor in the list is not on stage at this frame");
            if (std::find(vecActors.begin(), vecActors.end(), pactr) == vecActors.end())
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
        return req.Fail(kszErrBadState, "no actors on stage at this frame to arrange");

    std::string strLayout = "conversation";
    double dSpacing = kdSpacingDefault, xCentre = 0;
    if (!req.FArgString("layout", &strLayout, fFalse) || !req.FArgNumber("spacing", &dSpacing, fFalse) ||
        !req.FArgNumber("x", &xCentre, fFalse))
    {
        return fFalse;
    }
    strLayout = StrLower(strLayout);
    if (strLayout != "conversation" && strLayout != "line")
        return req.Fail(kszErrBadArgs, "'layout' must be \"conversation\" or \"line\"");
    if (dSpacing < 0.3 || dSpacing > 5)
        return req.Fail(kszErrBadArgs, "'spacing' must be between 0.3 and 5 metres");
    bool fConversation = strLayout == "conversation";
    bool fCoverage = fTrue;
    if (!req.FArgBool("coverage", &fCoverage, fFalse))
        return fFalse;

    StageFrame stage;
    CameraModel camera;
    if (!FGetStageFrame(pmvie, &stage, &camera))
        return FFailEngine(req, "no camera");
    RC rcView(0, 0, 544, 306);
    if (PmvuCur() != pvNil)
        PmvuCur()->GetRc(&rcView, cooLocal);

    // Where the picture's bottom centre meets the floor: some sets' cameras
    // look at floor well in front of their placement point
    Vec3 nearFloor(0, 0, 0);
    Vec3 worldNear;
    if (camera.FFloorAt(rcView.Dxp() / 2.0, rcView.Dyp() * kdNearFloorRow, &worldNear))
        nearFloor = stage.FromWorld(worldNear);
    if (!req.FHasArg("x"))
        xCentre = nearFloor.x;
    std::vector<double> vecDepths;
    for (double d : krgdDepth)
        vecDepths.push_back(nearFloor.z + d);
    if (req.FHasArg("z"))
    {
        double z;
        if (!req.FArgNumber("z", &z))
            return fFalse;
        vecDepths.assign(1, z);
    }

    // Keep copies so a failure leaves the actors as they were
    std::vector<PACTR> vecDups;
    for (PACTR pactr : vecActors)
    {
        PACTR pactrDup;
        if (!pactr->FDup(&pactrDup))
        {
            for (PACTR pactrT : vecDups)
                ReleasePpo(&pactrT);
            return FFailEngine(req, "could not arrange the actors");
        }
        vecDups.push_back(pactrDup);
    }
    auto restore = [&]() {
        for (size_t i = 0; i < vecActors.size(); i++)
            vecActors[i]->Restore(vecDups[i]);
    };

    std::vector<GroupSpot> vecSpots = VecGroupSpots(static_cast<int32_t>(vecActors.size()), dSpacing, fConversation);
    GroupSpot spotFocus = SpotGroupFocus(dSpacing);
    // Closest framing first; at each depth try sliding the group sideways
    // (furniture near the camera hides one side of the set) unless x is given
    struct Spot
    {
        double x, z;
        size_t idepth;
    };
    std::vector<Spot> vecSpotsTried;
    for (size_t idepth = 0; idepth < vecDepths.size(); idepth++)
    {
        for (double dx : krgdShift)
        {
            if (dx == 0 || !req.FHasArg("x"))
                vecSpotsTried.push_back({xCentre + dx, vecDepths[idepth], idepth});
        }
    }
    int32_t icamMain = pscen->Pbkgd()->Icam();
    // The first spot that works for this camera; then, among the good spots
    // just behind it, the one other cameras cover best
    size_t ispotGood = vecSpotsTried.size(), idepthGood = 0;
    int32_t scoreGood = -1;
    std::vector<std::vector<bool>> vecvecShowsGood;
    std::string strProblem;
    auto place = [&](double xFront, double zFront) -> bool {
        for (size_t i = 0; i < vecActors.size(); i++)
        {
            PACTR pactr = vecActors[i];
            Vec3 target = stage.ToWorld(Vec3(xFront + vecSpots[i].x, 0, zFront + vecSpots[i].z));
            if (!FTeleportActor(pactr, target, fTrue))
                return FFailEngine(req, "could not move an actor");
            Json jsonFace = Json("camera");
            if (fConversation && vecActors.size() > 1)
            {
                jsonFace = Json::MakeObject();
                jsonFace.Set("x", xFront + spotFocus.x);
                jsonFace.Set("z", zFront + spotFocus.z);
            }
            double degTurn;
            if (!FTurnToFace(req, pmvie, pactr, jsonFace, &degTurn) ||
                (std::fabs(degTurn) >= 0.5 && !pactr->FRotate(aZero, BraFromDeg(degTurn), aZero, fTrue)))
            {
                return req.Failed() ? fFalse : FFailEngine(req, "could not turn an actor");
            }
        }
        return fTrue;
    };
    auto fail = [&]() {
        restore();
        for (PACTR pactrT : vecDups)
            ReleasePpo(&pactrT);
        return fFalse;
    };
    for (size_t ispot = 0; ispot < vecSpotsTried.size(); ispot++)
    {
        const Spot &spot = vecSpotsTried[ispot];
        if (ispotGood < vecSpotsTried.size() && spot.idepth > idepthGood + kcDepthCoverage)
            break;
        if (!place(spot.x, spot.z))
            return fail();

        // Everyone fully in shot (legs may crop) and nobody hidden?
        EnsureRendered(pmvie);
        strProblem.clear();
        std::vector<ScreenBox> vecBoxes;
        std::vector<double> vecDistance;
        for (PACTR pactr : vecActors)
        {
            RC rc;
            if (!pactr->FIsInView())
            {
                strProblem = "someone is out of shot";
                break;
            }
            pactr->GetRcBounds(&rc);
            ScreenBox box{rc.xpLeft, rc.ypTop, rc.xpRight, rc.ypBottom};
            if (FCutOffAtSides(box, rcView.Dxp()))
            {
                strProblem = "someone is cut off by the frame";
                break;
            }
            BRS xr, yr, zr;
            pactr->Pbody()->GetPosition(&xr, &yr, &zr);
            vecBoxes.push_back(box);
            vecDistance.push_back((Vec3(DFromBrs(xr), DFromBrs(yr), DFromBrs(zr)) - camera.position).Length());
        }
        for (size_t i = 0; strProblem.empty() && i < vecBoxes.size(); i++)
        {
            for (size_t j = 0; j < vecBoxes.size(); j++)
            {
                if (vecDistance[j] < vecDistance[i] && DCoveredFraction(vecBoxes[i], vecBoxes[j]) > kdHiddenMaxArrange)
                {
                    strProblem = "someone is hidden behind someone else";
                    break;
                }
            }
        }
        for (size_t i = 0; strProblem.empty() && i < vecActors.size(); i++)
        {
            double dVisible;
            if (FVisibleFraction(pmvie, vecActors[i], &dVisible) && dVisible < kdVisibleMinArrange)
                strProblem = "someone is partly hidden";
        }
        if (!strProblem.empty())
            continue;
        if (ispotGood == vecSpotsTried.size())
            idepthGood = spot.idepth;
        if (!fCoverage)
        {
            ispotGood = ispot;
            break;
        }
        std::vector<std::vector<bool>> vecvecShows = _VecCoverage(pmvie, vecActors, rcView, icamMain);
        int32_t score = _CCoverageScore(vecvecShows);
        if (score > scoreGood)
        {
            ispotGood = ispot;
            scoreGood = score;
            vecvecShowsGood = vecvecShows;
        }
    }
    // Settle on the chosen spot (or the last tried, with the problem found)
    double xUsed = vecSpotsTried.back().x, zUsed = vecSpotsTried.back().z;
    if (ispotGood < vecSpotsTried.size())
    {
        strProblem.clear();
        xUsed = vecSpotsTried[ispotGood].x;
        zUsed = vecSpotsTried[ispotGood].z;
        if (!place(xUsed, zUsed))
            return fail();
    }

    for (size_t i = 0; i < vecActors.size(); i++)
    {
        if (!vecActors[i]->FCreateUndo(vecDups[i]))
            req.result.Set("warning", "arranged, but not every move could be added to the undo history");
        ReleasePpo(&vecDups[i]);
    }
    pmvie->SetDirty();
    RefreshViews(pmvie);
    EnsureRendered(pmvie);

    Json jsonList = Json::MakeArray();
    for (PACTR pactr : vecActors)
        jsonList.Push(JsonActor(pmvie, pactr, stage, camera));
    req.result.Set("layout", strLayout);
    req.result.Set("x", std::round(xUsed * 100) / 100);
    req.result.Set("z", zUsed);
    req.result.Set("actors", jsonList);
    if (!vecvecShowsGood.empty())
    {
        // The angles an editor can cut to (auto_camera uses them)
        Json jsonAngles = Json::MakeArray();
        for (size_t icam = 0; icam < vecvecShowsGood.size(); icam++)
        {
            Json jsonShows = Json::MakeArray();
            for (size_t i = 0; i < vecActors.size(); i++)
            {
                STN stn;
                if (vecvecShowsGood[icam][i])
                    jsonShows.Push(pmvie->FGetName(vecActors[i]->Arid(), &stn)
                                       ? Json(StrFromStn(&stn))
                                       : Json(static_cast<double>(vecActors[i]->Arid())));
            }
            if (jsonShows.Size() == 0)
                continue;
            Json json = Json::MakeObject();
            json.Set("camera", static_cast<int>(icam));
            json.Set("faces", jsonShows);
            jsonAngles.Push(json);
        }
        req.result.Set("other_angles", jsonAngles);
    }
    if (!strProblem.empty())
    {
        req.result.Set("framing", strProblem + " at every spot tried (up to z = " + _StrMetres(zUsed) + ")" +
                                      "; try another camera (suggest_camera), a smaller spacing, or fewer actors");
    }
    req.result.Set("note", "positions are this camera's stage coordinates; undo only reverts one actor, so "
                           "checkpoint first if you may want to go back");
    return fTrue;
}

const CommandInfo krgcmdBlock[] = {
    {"arrange_actors", _FCmdArrangeActors, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
