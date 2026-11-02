/***************************************************************************
    Automation bridge: a small rigid-body simulation for bouncing, thrown
    and falling actors (used by the physics command, see physicscmd.cpp).

    The movie engine has no physics; the bridge simulates here and bakes the
    result into ordinary per-frame moves, rotations and stretches, so the
    movie stays a normal .3mm file. Engine independent (standard library
    only) so it can be unit tested. Units: metres and seconds, stage axes
    (x right, y up, z away from the camera).
***************************************************************************/
#ifndef AUTOMATION_PHYSICS_H
#define AUTOMATION_PHYSICS_H

#include <cstdint>
#include <vector>

#include "stage.h"

namespace automation
{

struct PhysicsParams
{
    double gravity = 9.8;       // m/s^2, downward
    double restitution = 0.6;   // share of speed kept by a bounce (0 = thud, 1 = superball)
    double friction = 0.4;      // ground friction coefficient while touching the floor
    double airDrag = 0.05;      // share of speed lost per second in the air
    double radius = 0.25;       // the body's collision radius (metres)
    double framesPerSecond = 6; // movie frame rate
    int32_t substeps = 20;      // simulation steps per frame
    double maxSeconds = 20;     // stop after this long even if still moving
    double restSpeed = 0.15;    // m/s; slower than this on the floor counts as at rest
};

// Another actor the body can hit: a vertical cylinder on the floor
struct Obstacle
{
    double x, z;   // centre (stage metres)
    double radius; // metres
    double height; // metres
};

// The body's state at one movie frame
struct PhysicsFrame
{
    Vec3 position;        // of the body's base (y = 0 on the floor)
    Vec3 velocity;        // m/s
    bool fImpact;         // hit the floor or an obstacle during this frame
    double impactSpeed;   // speed into the surface at the hardest impact this frame
    int32_t iobstacleHit; // obstacle hit this frame, or -1
};

// Simulates from position/velocity. obstaclesAt(ifrm) gives the obstacles at
// each frame (others may move); pass an empty function for none. Returns
// one entry per frame starting with the initial state; stops once the body
// rests on the floor (that frame included) or after maxSeconds.
template <typename FN>
std::vector<PhysicsFrame> VecSimulate(const PhysicsParams &params, const Vec3 &position, const Vec3 &velocity,
                                      FN obstaclesAt);

// Initial velocity that throws a body from `from` to land at `to` (both on
// the floor or at their given heights) peaking `apexHeight` metres above
// the higher of the two
Vec3 VelocityToLand(const Vec3 &from, const Vec3 &to, double apexHeight, double gravity);

// A waypoint of a walk: where, and which of the caller's points it is (-1 for
// a detour the router added)
struct RouteStep
{
    Vec3 position;
    int32_t itarget;
};

// Bends a floor walk (stage metres) around standing actors: a leg that would
// pass through an obstacle gets a detour point beside it, on the side the
// leg was already closer to (the camera side, lower z, when it's close to a
// tie). Legs that start or end at an obstacle are left
// alone (the walker means to go up to it). radius is the walker's own.
std::vector<RouteStep> VecRouteAround(const Vec3 &start, const std::vector<Vec3> &targets,
                                      const std::vector<Obstacle> &obstacles, double radius);

// Steps the simulation by dt (exposed for VecSimulate and tests)
void StepPhysics(const PhysicsParams &params, const std::vector<Obstacle> &obstacles, double dt, Vec3 *pposition,
                 Vec3 *pvelocity, PhysicsFrame *pframe);

template <typename FN>
std::vector<PhysicsFrame> VecSimulate(const PhysicsParams &params, const Vec3 &position, const Vec3 &velocity,
                                      FN obstaclesAt)
{
    std::vector<PhysicsFrame> vecFrames;
    Vec3 pos = position, vel = velocity;
    vecFrames.push_back({pos, vel, false, 0, -1});
    int32_t cfrmMax = static_cast<int32_t>(params.maxSeconds * params.framesPerSecond);
    double dt = 1.0 / (params.framesPerSecond * params.substeps);
    for (int32_t ifrm = 1; ifrm <= cfrmMax; ifrm++)
    {
        std::vector<Obstacle> vecObstacles = obstaclesAt(ifrm);
        PhysicsFrame frame{pos, vel, false, 0, -1};
        for (int32_t istep = 0; istep < params.substeps; istep++)
            StepPhysics(params, vecObstacles, dt, &pos, &vel, &frame);
        frame.position = pos;
        frame.velocity = vel;
        vecFrames.push_back(frame);
        bool fOnFloor = pos.y <= 1e-6;
        if (fOnFloor && vel.Length() < params.restSpeed)
        {
            vecFrames.back().velocity = Vec3(0, 0, 0);
            break;
        }
    }
    return vecFrames;
}

} // namespace automation

#endif // AUTOMATION_PHYSICS_H
