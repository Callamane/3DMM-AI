/***************************************************************************
    Automation bridge: "stage" coordinates.

    Pure math, independent of the engine, so it can be unit tested.

    World space is BRender's: Y up, floor at y = 0, 10 world units per metre.
    The stage frame is defined per camera: origin at the camera's authored
    actor placement point (on the floor), +x = screen right, +z = away from
    the camera, units in metres.
***************************************************************************/
#ifndef AUTOMATION_STAGE_H
#define AUTOMATION_STAGE_H

#include <cstdint>
#include <vector>

namespace automation
{

const double kdwrPerMetre = 10.0;

struct Vec3
{
    double x = 0;
    double y = 0;
    double z = 0;

    Vec3() = default;
    Vec3(double xIn, double yIn, double zIn) : x(xIn), y(yIn), z(zIn)
    {
    }
    Vec3 operator+(const Vec3 &v) const
    {
        return Vec3(x + v.x, y + v.y, z + v.z);
    }
    Vec3 operator-(const Vec3 &v) const
    {
        return Vec3(x - v.x, y - v.y, z - v.z);
    }
    Vec3 operator*(double r) const
    {
        return Vec3(x * r, y * r, z * r);
    }
    double Dot(const Vec3 &v) const
    {
        return x * v.x + y * v.y + z * v.z;
    }
    double Length() const;
    Vec3 Normalized() const;
};

// Yaw of a direction on the floor: degrees about +y (counter-clockwise seen
// from above) that turn world +z to it. y is ignored.
double DegYaw(const Vec3 &dir);
// Wraps an angle to (-180, 180]
double DegWrap(double deg);
// Names an actor's facing, given degrees turned from facing the camera
// (+90 = facing screen right): "camera", "front-right", "right",
// "back-right", "away", "back-left", "left" or "front-left"
const char *PszFacing(double deg);

// Where actors in a group stand, in metres relative to the group's front
// centre (x right, z away). A conversation is a shallow arc bowed away from
// the camera so everyone stays visible; a line is straight.
struct GroupSpot
{
    double x, z;
};
std::vector<GroupSpot> VecGroupSpots(int32_t cactor, double dSpacing, bool fArc);
// The point a conversation group looks at: just in front of its centre,
// so partners face each other while turned a little toward the camera
GroupSpot SpotGroupFocus(double dSpacing);

// A camera as BRender describes it: camera-to-world matrix rows (X axis,
// Y axis, Z axis, position), vertical field of view, and viewport size.
// BRender cameras look down -Z.
struct CameraModel
{
    Vec3 axisX;
    Vec3 axisY;
    Vec3 axisZ;
    Vec3 position;
    double fovYDegrees = 60;
    double viewportWidth = 544;
    double viewportHeight = 306;
    double hither = 1;
    double yon = 1000;

    // Projects a world point to viewport pixels. Returns false if the point is
    // behind the camera. *pfInView is set if it lands inside the viewport.
    bool FProject(const Vec3 &world, double *pxp, double *pyp, bool *pfInView) const;

    // Casts a ray through viewport pixel (xp, yp) and intersects the floor
    // (y = 0). Returns false if the ray doesn't hit the floor in front.
    bool FFloorAt(double xp, double yp, Vec3 *pworld) const;
};

class StageFrame
{
  public:
    // placePoint: the authored actor placement point (world). If it is
    // (0,0,0) the caller should pass a point in front of the camera instead.
    void Init(const CameraModel &camera, const Vec3 &placePoint);

    // Stage (metres, x right, y up, z away) <-> world.
    Vec3 ToWorld(const Vec3 &stage) const;
    Vec3 FromWorld(const Vec3 &world) const;
    // Directions (no translation)
    Vec3 DirToWorld(const Vec3 &stageDir) const;

    // Yaw (degrees, rotation about +y) of the stage +z axis in world space.
    // Used to convert "facing" angles.
    double YawOfStageForwardDegrees() const;

    const Vec3 &Origin() const
    {
        return _origin;
    }
    const Vec3 &Right() const
    {
        return _right;
    }
    const Vec3 &Forward() const
    {
        return _forward;
    }

  private:
    Vec3 _origin;
    Vec3 _right{1, 0, 0};
    Vec3 _forward{0, 0, -1};
};

} // namespace automation

#endif // AUTOMATION_STAGE_H
