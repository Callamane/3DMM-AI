/***************************************************************************
    Automation bridge: stage coordinate math.
***************************************************************************/
#include "stage.h"

#include <cmath>

namespace automation
{

const double kPi = 3.14159265358979323846;

double Vec3::Length() const
{
    return std::sqrt(x * x + y * y + z * z);
}

Vec3 Vec3::Normalized() const
{
    double r = Length();
    if (r < 1e-9)
        return Vec3();
    return Vec3(x / r, y / r, z / r);
}

bool CameraModel::FProject(const Vec3 &world, double *pxp, double *pyp, bool *pfInView) const
{
    Vec3 d = world - position;
    double xc = d.Dot(axisX);
    double yc = d.Dot(axisY);
    double zc = d.Dot(axisZ);

    *pfInView = false;
    if (zc > -1e-6)
        return false; // behind (or at) the camera

    double scale = 1.0 / std::tan(fovYDegrees * kPi / 360.0);
    double aspect = viewportWidth / viewportHeight;
    double xNdc = (xc * scale / aspect) / -zc;
    double yNdc = (yc * scale) / -zc;

    *pxp = (xNdc + 1.0) * 0.5 * viewportWidth;
    *pyp = (1.0 - yNdc) * 0.5 * viewportHeight;
    *pfInView = *pxp >= 0 && *pxp < viewportWidth && *pyp >= 0 && *pyp < viewportHeight;
    return true;
}

bool CameraModel::FFloorAt(double xp, double yp, Vec3 *pworld) const
{
    double scale = 1.0 / std::tan(fovYDegrees * kPi / 360.0);
    double aspect = viewportWidth / viewportHeight;
    double xNdc = xp / viewportWidth * 2.0 - 1.0;
    double yNdc = 1.0 - yp / viewportHeight * 2.0;

    // Direction in camera space for z = -1, then to world
    Vec3 dirCam(xNdc * aspect / scale, yNdc / scale, -1.0);
    Vec3 dir = axisX * dirCam.x + axisY * dirCam.y + axisZ * dirCam.z;
    if (dir.y > -1e-9)
        return false; // parallel to or pointing away from the floor
    double t = -position.y / dir.y;
    if (t <= 0)
        return false;
    *pworld = position + dir * t;
    pworld->y = 0;
    return true;
}

void StageFrame::Init(const CameraModel &camera, const Vec3 &placePoint)
{
    Vec3 right(camera.axisX.x, 0, camera.axisX.z);
    // Camera looks down -Z, so "into the picture" is -axisZ
    Vec3 forward(-camera.axisZ.x, 0, -camera.axisZ.z);

    right = right.Normalized();
    forward = forward.Normalized();
    if (forward.Length() < 0.5)
    {
        // Camera looking straight down: use its up vector as "away"
        forward = Vec3(camera.axisY.x, 0, camera.axisY.z).Normalized();
    }
    if (right.Length() < 0.5)
        right = Vec3(forward.z, 0, -forward.x);

    // Make right exactly perpendicular to forward on the floor plane
    right = (right - forward * right.Dot(forward)).Normalized();

    _right = right;
    _forward = forward;
    _origin = Vec3(placePoint.x, 0, placePoint.z);
}

Vec3 StageFrame::DirToWorld(const Vec3 &stageDir) const
{
    Vec3 world = _right * (stageDir.x * kdwrPerMetre) + _forward * (stageDir.z * kdwrPerMetre);
    world.y = stageDir.y * kdwrPerMetre;
    return world;
}

Vec3 StageFrame::ToWorld(const Vec3 &stage) const
{
    return _origin + DirToWorld(stage);
}

Vec3 StageFrame::FromWorld(const Vec3 &world) const
{
    Vec3 d = world - _origin;
    return Vec3(d.Dot(_right) / kdwrPerMetre, world.y / kdwrPerMetre, d.Dot(_forward) / kdwrPerMetre);
}

double DegYaw(const Vec3 &dir)
{
    // Rotating (0,0,1) by angle a about y gives (sin a, 0, cos a)
    return std::atan2(dir.x, dir.z) * 180.0 / kPi;
}

double DegWrap(double deg)
{
    deg = std::fmod(deg, 360.0);
    if (deg <= -180)
        deg += 360;
    else if (deg > 180)
        deg -= 360;
    return deg;
}

const char *PszFacing(double deg)
{
    static const char *const rgpsz[] = {"camera", "front-right", "right", "back-right",
                                        "away",   "back-left",   "left",  "front-left"};
    double degPos = DegWrap(deg);
    if (degPos < 0)
        degPos += 360;
    return rgpsz[static_cast<int>(std::floor(degPos / 45.0 + 0.5)) % 8];
}

std::vector<GroupSpot> VecGroupSpots(int32_t cactor, double dSpacing, bool fArc)
{
    std::vector<GroupSpot> vecSpots;
    double dxHalf = (cactor - 1) * dSpacing / 2;
    for (int32_t i = 0; i < cactor; i++)
    {
        double x = i * dSpacing - dxHalf;
        double z = 0;
        if (fArc && cactor > 2)
        {
            double t = x / dxHalf;
            z = 0.4 * dSpacing * (1 - t * t);
        }
        vecSpots.push_back({x, z});
    }
    return vecSpots;
}

GroupSpot SpotGroupFocus(double dSpacing)
{
    // Two actors dSpacing apart then turn about 30 degrees toward the camera
    return {0, -0.3 * dSpacing};
}

double StageFrame::YawOfStageForwardDegrees() const
{
    // Rotation about +y that takes world +z (0,0,1) to _forward.
    // Rotating (0,0,1) by angle a about y gives (sin a, 0, cos a).
    return std::atan2(_forward.x, _forward.z) * 180.0 / kPi;
}

} // namespace automation
