/***************************************************************************
    Automation bridge: arrange_actors, blocking a group of actors in one
    call. A conversation stands the actors on a shallow arc, each turned
    toward a point just in front of the group, so partners face each other
    while staying open to the camera; a line stands them in a row facing the
    camera. Unless told the depth, the group starts close to the camera and
    steps back until everyone is fully in shot and nobody hides anybody.
***************************************************************************/
#include "bridge.h"
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
    std::vector<std::pair<double, double>> vecSpotsTried;
    for (double z : vecDepths)
    {
        for (double dx : krgdShift)
        {
            if (dx == 0 || !req.FHasArg("x"))
                vecSpotsTried.push_back({xCentre + dx, z});
        }
    }
    double xUsed = xCentre, zUsed = vecDepths.back();
    std::string strProblem;
    for (const auto &spot : vecSpotsTried)
    {
        double xFront = spot.first, zFront = spot.second;
        xUsed = xFront;
        zUsed = zFront;
        for (size_t i = 0; i < vecActors.size(); i++)
        {
            PACTR pactr = vecActors[i];
            Vec3 target = stage.ToWorld(Vec3(xFront + vecSpots[i].x, 0, zFront + vecSpots[i].z));
            if (!FTeleportActor(pactr, target, fTrue))
            {
                restore();
                for (PACTR pactrT : vecDups)
                    ReleasePpo(&pactrT);
                return FFailEngine(req, "could not move an actor");
            }
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
                restore();
                for (PACTR pactrT : vecDups)
                    ReleasePpo(&pactrT);
                return req.Failed() ? fFalse : FFailEngine(req, "could not turn an actor");
            }
        }

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
        if (strProblem.empty())
            break;
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
