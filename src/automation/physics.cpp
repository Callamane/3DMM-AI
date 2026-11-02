/***************************************************************************
    Automation bridge: rigid-body simulation (see physics.h).
***************************************************************************/
#include "physics.h"

#include <algorithm>
#include <cmath>

namespace automation
{

const double kdSpeedImpactMin = 0.5; // m/s; gentler contacts aren't reported as impacts
const double kdSpeedBounceMin = 0.3; // m/s; slower landings stop instead of bouncing

static void _NoteImpact(PhysicsFrame *pframe, double dSpeed, int32_t iobstacle)
{
    if (dSpeed < kdSpeedImpactMin)
        return;
    pframe->fImpact = true;
    if (dSpeed > pframe->impactSpeed)
    {
        pframe->impactSpeed = dSpeed;
        pframe->iobstacleHit = iobstacle;
    }
}

void StepPhysics(const PhysicsParams &params, const std::vector<Obstacle> &obstacles, double dt, Vec3 *pposition,
                 Vec3 *pvelocity, PhysicsFrame *pframe)
{
    Vec3 &pos = *pposition;
    Vec3 &vel = *pvelocity;
    bool fOnFloor = pos.y <= 1e-9 && vel.y <= 1e-9;

    if (!fOnFloor)
    {
        vel.y -= params.gravity * dt;
        vel = vel * std::max(0.0, 1 - params.airDrag * dt);
    }
    else
    {
        // Sliding: friction slows the body along the floor
        vel.y = 0;
        double dSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        double dSlow = params.friction * params.gravity * dt;
        double rKeep = dSpeed > dSlow ? (dSpeed - dSlow) / dSpeed : 0;
        vel.x *= rKeep;
        vel.z *= rKeep;
    }
    pos = pos + vel * dt;

    // The floor
    if (pos.y < 0)
    {
        double dSpeedIn = -vel.y;
        pos.y = 0;
        _NoteImpact(pframe, dSpeedIn, -1);
        if (dSpeedIn * params.restitution > kdSpeedBounceMin)
            vel.y = dSpeedIn * params.restitution;
        else
            vel.y = 0;
        // A bounce also scuffs off some sideways speed
        double rKeep = std::max(0.0, 1 - params.friction * (1 + params.restitution) * dSpeedIn /
                                             std::max(1e-6, std::sqrt(vel.x * vel.x + vel.z * vel.z)));
        vel.x *= rKeep;
        vel.z *= rKeep;
    }

    // Other actors: vertical cylinders
    for (size_t iobs = 0; iobs < obstacles.size(); iobs++)
    {
        const Obstacle &obs = obstacles[iobs];
        if (pos.y >= obs.height)
            continue;
        double dx = pos.x - obs.x, dz = pos.z - obs.z;
        double dist = std::sqrt(dx * dx + dz * dz);
        double distMin = params.radius + obs.radius;
        if (dist >= distMin)
            continue;
        // Push out along the line between centres (any direction if exactly
        // on top of each other)
        double nx = dist > 1e-9 ? dx / dist : 1, nz = dist > 1e-9 ? dz / dist : 0;
        pos.x = obs.x + nx * distMin;
        pos.z = obs.z + nz * distMin;
        double vn = vel.x * nx + vel.z * nz;
        if (vn < 0)
        {
            _NoteImpact(pframe, -vn, static_cast<int32_t>(iobs));
            vel.x -= (1 + params.restitution) * vn * nx;
            vel.z -= (1 + params.restitution) * vn * nz;
        }
    }
}

Vec3 VelocityToLand(const Vec3 &from, const Vec3 &to, double apexHeight, double gravity)
{
    double yApex = std::max(from.y, to.y) + std::max(0.0, apexHeight);
    double vy = std::sqrt(2 * gravity * (yApex - from.y));
    double dtUp = vy / gravity;
    double dtDown = std::sqrt(2 * (yApex - to.y) / gravity);
    double dt = std::max(1e-6, dtUp + dtDown);
    return Vec3((to.x - from.x) / dt, vy, (to.z - from.z) / dt);
}

const double kdRouteMargin = 0.15; // metres of clearance when walking past someone
const int32_t kcdetourMax = 20;
const double kdRouteTieShare = 0.3; // nearer the centre than this share of the clearance is a tie

std::vector<RouteStep> VecRouteAround(const Vec3 &start, const std::vector<Vec3> &targets,
                                      const std::vector<Obstacle> &obstacles, double radius)
{
    std::vector<RouteStep> vecSteps;
    for (size_t i = 0; i < targets.size(); i++)
        vecSteps.push_back({targets[i], static_cast<int32_t>(i)});

    int32_t cdetour = 0;
    // A detour is inserted before the leg's end and the new leg into it is
    // checked next, so the index only advances past legs that are clear
    size_t istep = 0;
    while (istep < vecSteps.size() && cdetour < kcdetourMax)
    {
        Vec3 a = istep == 0 ? start : vecSteps[istep - 1].position;
        Vec3 b = vecSteps[istep].position;
        double dx = b.x - a.x, dz = b.z - a.z;
        double dLen2 = dx * dx + dz * dz;
        if (dLen2 < 1e-9)
        {
            istep++;
            continue;
        }
        // The obstacle this leg passes deepest through
        int32_t iobsWorst = -1;
        double dDepthWorst = 0, tWorst = 0;
        for (size_t iobs = 0; iobs < obstacles.size(); iobs++)
        {
            const Obstacle &obs = obstacles[iobs];
            double dClear = obs.radius + radius + kdRouteMargin;
            if (std::hypot(a.x - obs.x, a.z - obs.z) < dClear || std::hypot(b.x - obs.x, b.z - obs.z) < dClear)
                continue;
            double t = std::clamp(((obs.x - a.x) * dx + (obs.z - a.z) * dz) / dLen2, 0.0, 1.0);
            double dDist = std::hypot(a.x + t * dx - obs.x, a.z + t * dz - obs.z);
            if (dClear - dDist > dDepthWorst)
            {
                dDepthWorst = dClear - dDist;
                iobsWorst = static_cast<int32_t>(iobs);
                tWorst = t;
            }
        }
        if (iobsWorst < 0)
        {
            istep++;
            continue;
        }

        // Step out sideways from the obstacle, past its edge
        const Obstacle &obs = obstacles[iobsWorst];
        double dClear = (obs.radius + radius + kdRouteMargin) * 1.15;
        double nx = a.x + tWorst * dx - obs.x, nz = a.z + tWorst * dz - obs.z;
        double dN = std::hypot(nx, nz);
        if (dN < kdRouteTieShare * dClear)
        {
            // Close to dead centre: go round on the camera side (lower z),
            // like an actor crossing downstage, so the walker stays in view
            double dLen = std::sqrt(dLen2);
            nx = -dz / dLen;
            nz = dx / dLen;
            if (nz > 0 || (nz == 0 && nx > 0))
            {
                nx = -nx;
                nz = -nz;
            }
        }
        else
        {
            nx /= dN;
            nz /= dN;
        }
        Vec3 detour(obs.x + nx * dClear, 0, obs.z + nz * dClear);
        vecSteps.insert(vecSteps.begin() + istep, {detour, -1});
        cdetour++;
    }
    return vecSteps;
}

} // namespace automation
