/***************************************************************************
    Automation bridge: physics, making an actor drop, get thrown or launched
    and bounce, slide and settle, knocking off other actors on the way.

    The engine has no physics. physics.cpp simulates the motion; this file
    bakes it into the movie with the Studio's own per-frame edits: the actor
    holds its action in place, then each frame gets a "move from here"
    (ACTR::FMoveRoute, an aetMove event), optional spin (FRotate) and a
    squash-and-stretch on hard landings (FPull, the Studio's squash tool).
    The result is an ordinary .3mm that plays the same in any 3D Movie Maker.
***************************************************************************/
#include "bridge.h"
#include "physics.h"

#include <cmath>
#include <map>

ASSERTNAME

namespace automation
{

const double kdDropHeightDefault = 1.5; // metres
const double kdApexDefault = 1.0;       // metres above the start for a throw
const double kdTumbleAirDegPerSec = 400;
const double kdTwirlDegPerSec = 360;
const double kdSquashPerSpeed = 0.06; // squash amount per m/s of impact
const double kdSquashMax = 0.35;

void GetActorFootprint(PACTR pactr, double *pdRadius, double *pdHeight)
{
    BCB bcb;
    pactr->Pbody()->GetBcbBounds(&bcb, fTrue);
    double dx = DFromBrs(BrsSub(bcb.xrMax, bcb.xrMin)) / kdwrPerMetre;
    double dz = DFromBrs(BrsSub(bcb.zrMax, bcb.zrMin)) / kdwrPerMetre;
    // Bounds include outstretched arms; the body itself is narrower
    *pdRadius = LwMax(1, static_cast<int32_t>((dx + dz) / 4 * 0.8 * 1000)) / 1000.0;
    *pdHeight = DFromBrs(BrsSub(bcb.yrMax, bcb.yrMin)) / kdwrPerMetre;
}

static Vec3 _PosWorld(PACTR pactr)
{
    BRS xr, yr, zr;
    pactr->Pbody()->GetPosition(&xr, &yr, &zr);
    return Vec3(DFromBrs(xr), DFromBrs(yr), DFromBrs(zr));
}

static bool _FParseVec(Request &req, const Json &json, Vec3 *pv)
{
    if (!json.IsObject())
        return req.Fail(kszErrBadArgs, "'velocity' must be {x, y, z} in metres per second (x right, y up, z away)");
    for (const char *psz : {"x", "y", "z"})
    {
        const Json &jsonT = json.Get(psz);
        if (!jsonT.IsNull() && !jsonT.IsNumber())
            return req.Fail(kszErrBadArgs, std::string("'velocity.") + psz + "' must be a number");
    }
    *pv = Vec3(json.Get("x").IsNumber() ? json.Get("x").AsNumber() : 0,
               json.Get("y").IsNumber() ? json.Get("y").AsNumber() : 0,
               json.Get("z").IsNumber() ? json.Get("z").AsNumber() : 0);
    return fTrue;
}

static bool _FCmdPhysics(Request &req)
{
    PACTR pactr;
    if (!FResolveActor(req, "actor", &pactr))
        return fFalse;
    PSCEN pscen = pactr->Pscen();
    PMVIE pmvie = pscen->Pmvie();
    if (!FRequireNotPlaying(req, pmvie) || !FApplyFrameArg(req, pscen))
        return fFalse;
    if (!pactr->FOnStage())
        return req.Fail(kszErrBadState, "the actor is not on stage at frame " + std::to_string(pscen->Nfrm()));

    PhysicsParams params;
    params.framesPerSecond = kfps;
    std::string strMotion, strSpin = "none";
    bool fSquash = fTrue, fCollide = fTrue;
    double dBounce = 0.5, dHeight = -1;
    if (!req.FArgString("motion", &strMotion, fFalse) || !req.FArgString("spin", &strSpin, fFalse) ||
        !req.FArgBool("squash", &fSquash, fFalse) || !req.FArgBool("collide", &fCollide, fFalse) ||
        !req.FArgNumber("bounciness", &dBounce, fFalse) || !req.FArgNumber("friction", &params.friction, fFalse) ||
        !req.FArgNumber("height", &dHeight, fFalse) || !req.FArgNumber("max_seconds", &params.maxSeconds, fFalse))
    {
        return fFalse;
    }
    strMotion = StrLower(strMotion);
    strSpin = StrLower(strSpin);
    if (strMotion.empty())
        strMotion = req.FHasArg("to") ? "throw" : (req.FHasArg("velocity") ? "launch" : "drop");
    if (strMotion != "drop" && strMotion != "throw" && strMotion != "launch")
        return req.Fail(kszErrBadArgs, "'motion' must be \"drop\", \"throw\" or \"launch\"");
    if (strSpin != "none" && strSpin != "tumble" && strSpin != "twirl")
        return req.Fail(kszErrBadArgs, "'spin' must be \"none\", \"tumble\" or \"twirl\"");
    if (dBounce < 0 || dBounce > 0.95)
        return req.Fail(kszErrBadArgs, "'bounciness' must be between 0 (thud) and 0.95 (superball)");
    if (params.friction < 0 || params.friction > 2)
        return req.Fail(kszErrBadArgs, "'friction' must be between 0 (ice) and 2");
    if (params.maxSeconds <= 0 || params.maxSeconds > 30)
        return req.Fail(kszErrBadArgs, "'max_seconds' must be between 0 and 30");
    params.restitution = dBounce;

    int32_t anid = pactr->AnidCur();
    if (req.FHasArg("action") && !FResolveAction(req, pactr->Ptmpl(), req.args.Get("action"), &anid))
        return fFalse;

    StageFrame stage;
    CameraModel camera;
    if (!FGetStageFrame(pmvie, &stage, &camera))
        return FFailEngine(req, "no camera");
    EnsureRendered(pmvie);
    int32_t nfrmStart = pscen->Nfrm();
    Vec3 start = stage.FromWorld(_PosWorld(pactr));
    double dBodyHeight;
    GetActorFootprint(pactr, &params.radius, &dBodyHeight);

    // Starting velocity
    Vec3 velocity;
    if (strMotion == "drop")
    {
        if (dHeight < 0 && start.y < 0.01)
            dHeight = kdDropHeightDefault;
        if (dHeight >= 0)
            start.y = dHeight;
        if (start.y < 0.01)
            return req.Fail(kszErrBadArgs, "nothing to drop: give a 'height' above the floor");
    }
    else if (strMotion == "launch")
    {
        if (!_FParseVec(req, req.args.Get("velocity"), &velocity))
            return fFalse;
    }
    else
    {
        Vec3 target;
        const Json &jsonTo = req.args.Get("to");
        if (jsonTo.IsObject())
        {
            Vec3 world;
            if (!FParsePosition(req, jsonTo, stage, &world))
                return fFalse;
            target = stage.FromWorld(world);
        }
        else
        {
            Request reqT;
            reqT.args.Set("actor", jsonTo);
            PACTR pactrTarget;
            if (!FResolveActor(reqT, "actor", &pactrTarget))
                return req.Fail(kszErrBadArgs, "'to' must be a point {x, z} or an actor: " + reqT.errMessage);
            if (pactrTarget == pactr)
                return req.Fail(kszErrBadArgs, "an actor can't be thrown at itself");
            target = stage.FromWorld(_PosWorld(pactrTarget));
        }
        target.y = 0;
        velocity = VelocityToLand(start, target, dHeight >= 0 ? dHeight : kdApexDefault, params.gravity);
    }

    // Everyone else is an obstacle, wherever they are at each frame
    int32_t cfrmMax = static_cast<int32_t>(params.maxSeconds * kfps) + 1;
    std::vector<std::vector<Obstacle>> vecvecObstacles;
    std::vector<std::vector<PACTR>> vecvecObstacleActors;
    PGL pglpactr = pscen->PglRollCall();
    for (int32_t ifrm = 0; fCollide && ifrm <= cfrmMax && nfrmStart + ifrm <= pscen->NfrmLast(); ifrm++)
    {
        if (!pscen->FGotoFrm(nfrmStart + ifrm))
            return FFailEngine(req, "could not step through the scene");
        EnsureRendered(pmvie);
        std::vector<Obstacle> vecObstacles;
        std::vector<PACTR> vecActors;
        for (int32_t ipactr = 0; pglpactr != pvNil && ipactr < pglpactr->IvMac(); ipactr++)
        {
            PACTR pactrOther;
            pglpactr->Get(ipactr, &pactrOther);
            if (pactrOther == pactr || !pactrOther->FOnStage())
                continue;
            Obstacle obs;
            Vec3 pos = stage.FromWorld(_PosWorld(pactrOther));
            GetActorFootprint(pactrOther, &obs.radius, &obs.height);
            obs.x = pos.x;
            obs.z = pos.z;
            obs.height += pos.y;
            vecObstacles.push_back(obs);
            vecActors.push_back(pactrOther);
        }
        vecvecObstacles.push_back(vecObstacles);
        vecvecObstacleActors.push_back(vecActors);
    }
    if (!pscen->FGotoFrm(nfrmStart))
        return FFailEngine(req, "could not go back to frame " + std::to_string(nfrmStart));
    auto obstaclesAt = [&](int32_t ifrm) {
        if (vecvecObstacles.empty())
            return std::vector<Obstacle>();
        return vecvecObstacles[LwMin(ifrm, static_cast<int32_t>(vecvecObstacles.size()) - 1)];
    };
    std::vector<PhysicsFrame> vecFrames = VecSimulate(params, start, velocity, obstaclesAt);
    int32_t cfrm = static_cast<int32_t>(vecFrames.size()) - 1;
    if (cfrm < 1)
        return req.Fail(kszErrBadArgs, "nothing moves: the actor is already at rest");

    // Bake it into the movie
    PACTR pactrDup;
    if (!pactr->FDup(&pactrDup))
        return FFailEngine(req, "could not start the edit");
    auto fail = [&](const std::string &strWhat) {
        pactr->Restore(pactrDup);
        ReleasePpo(&pactrDup);
        pscen->FGotoFrm(nfrmStart);
        RefreshViews(pmvie);
        return req.Failed() ? fFalse : FFailEngine(req, strWhat);
    };
    if (!FHoldInPlace(req, pactr, anid, cfrm))
        return fail("could not hold the actor in place");
    if (!pscen->FGotoFrm(nfrmStart))
        return fail("could not go back to the start frame");

    // Tumbling rolls forward along the direction of travel, so face it first
    Vec3 travel = vecFrames.back().position - vecFrames.front().position;
    if (strSpin == "tumble" && std::fabs(travel.x) + std::fabs(travel.z) > 0.05)
    {
        // A point along the travel from where it stands now (the start
        // point may be in the air)
        Vec3 posNow = stage.FromWorld(_PosWorld(pactr));
        Json jsonFace = Json::MakeObject();
        jsonFace.Set("x", posNow.x + travel.x);
        jsonFace.Set("z", posNow.z + travel.z);
        double degTurn;
        if (!FTurnToFace(req, pmvie, pactr, jsonFace, &degTurn))
            return fail("could not turn the actor");
        if (std::fabs(degTurn) >= 0.5 && !pactr->FRotate(aZero, BraFromDeg(degTurn), aZero, fTrue))
            return fail("could not turn the actor");
    }

    // Squash on hard landings: squashed on the impact frame, stretched a
    // little the next, back to normal after
    std::map<int32_t, Vec3> mapScale;
    for (int32_t ifrm = 1; fSquash && ifrm <= cfrm; ifrm++)
    {
        const PhysicsFrame &frame = vecFrames[ifrm];
        if (!frame.fImpact || frame.iobstacleHit >= 0)
            continue;
        double d = LwMin(static_cast<int32_t>(frame.impactSpeed * kdSquashPerSpeed * 1000),
                         static_cast<int32_t>(kdSquashMax * 1000)) /
                   1000.0;
        mapScale[ifrm] = Vec3(1 + d / 2, 1 - d, 1 + d / 2);
        if (!mapScale.count(ifrm + 1))
            mapScale[ifrm + 1] = Vec3(1 - d / 4, 1 + d / 3, 1 - d / 4);
        if (!mapScale.count(ifrm + 2))
            mapScale[ifrm + 2] = Vec3(1, 1, 1);
    }

    Vec3 scaleCur(1, 1, 1);
    Json jsonImpacts = Json::MakeArray();
    for (int32_t ifrm = 0; ifrm <= cfrm; ifrm++)
    {
        if (!pscen->FGotoFrm(nfrmStart + ifrm))
            return fail("could not go to frame " + std::to_string(nfrmStart + ifrm));
        const PhysicsFrame &frame = vecFrames[ifrm];
        Vec3 delta = stage.ToWorld(frame.position) - _PosWorld(pactr);
        bool fMoved;
        if (delta.Length() > 1e-3 &&
            !pactr->FMoveRoute(BrsFromD(delta.x), BrsFromD(delta.y), BrsFromD(delta.z), &fMoved, fmafNil))
        {
            return fail("could not move the actor");
        }

        if (ifrm > 0 && strSpin != "none")
        {
            const Vec3 &prev = vecFrames[ifrm - 1].position;
            double dHoriz = std::hypot(frame.position.x - prev.x, frame.position.z - prev.z);
            double deg = 0;
            if (strSpin == "twirl")
                deg = dHoriz > 1e-3 || frame.position.y > 0 ? kdTwirlDegPerSec / kfps : 0;
            else if (frame.position.y > 0.01)
                deg = kdTumbleAirDegPerSec / kfps;
            else
                deg = dHoriz / params.radius * 180 / 3.14159265358979;
            if (deg > 0.5)
            {
                bool fOk = strSpin == "twirl" ? pactr->FRotate(aZero, BraFromDeg(deg), aZero, fTrue)
                                              : pactr->FRotate(aZero, aZero, BraFromDeg(deg), fTrue);
                if (!fOk)
                    return fail("could not spin the actor");
            }
        }

        auto it = mapScale.find(ifrm);
        if (it != mapScale.end())
        {
            const Vec3 &want = it->second;
            if (!pactr->FPull(BrsFromD(want.x / scaleCur.x), BrsFromD(want.y / scaleCur.y),
                              BrsFromD(want.z / scaleCur.z)))
            {
                return fail("could not squash the actor");
            }
            scaleCur = want;
        }

        if (frame.fImpact)
        {
            Json json = Json::MakeObject();
            json.Set("frame", nfrmStart + ifrm);
            json.Set("speed", std::round(frame.impactSpeed * 10) / 10);
            std::string strHit = "floor";
            if (frame.iobstacleHit >= 0)
            {
                const std::vector<PACTR> &vecActors =
                    vecvecObstacleActors[LwMin(ifrm, static_cast<int32_t>(vecvecObstacleActors.size()) - 1)];
                STN stn;
                PACTR pactrHit = vecActors[frame.iobstacleHit];
                strHit = pmvie->FGetName(pactrHit->Arid(), &stn) ? StrFromStn(&stn)
                                                                 : "actor " + std::to_string(pactrHit->Arid());
            }
            json.Set("hit", strHit);
            jsonImpacts.Push(json);
        }
    }

    if (!pactr->FCreateUndo(pactrDup))
        req.result.Set("warning", "the motion was added but could not be added to the undo history");
    ReleasePpo(&pactrDup);
    pscen->FGotoFrm(nfrmStart);
    pmvie->SetDirty();
    RefreshViews(pmvie);

    const Vec3 &rest = vecFrames.back().position;
    Json jsonRest = Json::MakeObject();
    jsonRest.Set("x", std::round(rest.x * 100) / 100);
    jsonRest.Set("z", std::round(rest.z * 100) / 100);
    req.result.Set("motion", strMotion);
    req.result.Set("start_frame", nfrmStart);
    req.result.Set("end_frame", nfrmStart + cfrm);
    req.result.Set("frames", cfrm);
    req.result.Set("impacts", jsonImpacts);
    req.result.Set("comes_to_rest_at", jsonRest);
    req.result.Set("note", "attach_sound at the impact frames for bumps; positions are this camera's stage "
                           "coordinates; recording replaces what the actor did after the start frame");
    return fTrue;
}

const CommandInfo krgcmdPhysics[] = {
    {"physics", _FCmdPhysics, fTrue},
    {nullptr, nullptr, fFalse},
};

} // namespace automation
