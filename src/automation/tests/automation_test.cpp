// Unit tests for the engine-independent parts of the automation bridge.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "directing.h"
#include "image8.h"
#include "json.h"
#include "physics.h"
#include "pngwrite.h"
#include "review.h"
#include "stage.h"

using namespace automation;

TEST(AutomationJson, ParsesRequest)
{
    Json json;
    std::string strError;
    ASSERT_TRUE(
        Json::FParse(R"({"id": 7, "cmd": "add_actor", "args": {"template": "Ron", "position": {"x": -1.5, "z": 2}}})",
                     &json, &strError))
        << strError;
    EXPECT_EQ(json.Get("id").AsNumber(), 7);
    EXPECT_EQ(json.Get("cmd").AsString(), "add_actor");
    EXPECT_EQ(json.Get("args").Get("template").AsString(), "Ron");
    EXPECT_DOUBLE_EQ(json.Get("args").Get("position").Get("x").AsNumber(), -1.5);
    EXPECT_TRUE(json.Get("missing").IsNull());
    EXPECT_TRUE(json.Get("args").Get("nope").Get("deeper").IsNull());
}

TEST(AutomationJson, RoundTripsStringsAndUnicode)
{
    Json json;
    std::string strError;
    ASSERT_TRUE(
        Json::FParse(R"(["a\"b\\c\n", "caf\u00e9", "\ud83c\udfac", true, false, null, 1e3, -0.25])", &json, &strError))
        << strError;
    ASSERT_EQ(json.Size(), 8u);
    EXPECT_EQ(json.AsArray()[0].AsString(), "a\"b\\c\n");
    EXPECT_EQ(json.AsArray()[1].AsString(), "caf\xc3\xa9");
    EXPECT_EQ(json.AsArray()[2].AsString(), "\xf0\x9f\x8e\xac");
    EXPECT_EQ(json.Dump(), R"(["a\"b\\c\n","café","🎬",true,false,null,1000,-0.25])");
}

TEST(AutomationJson, RejectsMalformed)
{
    Json json;
    std::string strError;
    for (const char *psz :
         {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "[1] 2", "\"\\ud800\"", "{1:2}"})
    {
        EXPECT_FALSE(Json::FParse(psz, &json, &strError)) << psz;
        EXPECT_FALSE(strError.empty());
    }
}

TEST(AutomationJson, LimitsNesting)
{
    Json json;
    std::string strError;
    std::string str(1000, '[');
    str += std::string(1000, ']');
    EXPECT_FALSE(Json::FParse(str, &json, &strError));
}

TEST(AutomationJson, BuildsObjectsInOrder)
{
    Json json;
    json.Set("b", 1);
    json.Set("a", "x");
    json.Set("b", 2);
    Json arr;
    arr.Push(Json());
    arr.Push(3.5);
    json.Set("list", arr);
    EXPECT_EQ(json.Dump(), R"({"b":2,"a":"x","list":[null,3.5]})");
}

TEST(AutomationPng, EncodesValidStructure)
{
    std::vector<uint8_t> pixels = {0, 1, 2, 0, 1, 2, 2, 1, 0, 9, 9, 9};
    std::vector<PngColor> palette = {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}};
    std::vector<uint8_t> png = EncodePngIndexed(pixels.data(), 3, 2, 6, palette);

    const uint8_t rgbSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    ASSERT_GT(png.size(), 8u);
    EXPECT_EQ(memcmp(png.data(), rgbSig, 8), 0);
    // IHDR immediately follows: length 13, width 3, height 2, depth 8, type 3
    EXPECT_EQ(png[11], 13);
    EXPECT_EQ(std::string(png.begin() + 12, png.begin() + 16), "IHDR");
    EXPECT_EQ(png[19], 3);
    EXPECT_EQ(png[23], 2);
    EXPECT_EQ(png[24], 8);
    EXPECT_EQ(png[25], 3);
    // Ends with IEND and its fixed CRC
    const uint8_t rgbEnd[8] = {'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    EXPECT_EQ(memcmp(png.data() + png.size() - 8, rgbEnd, 8), 0);
}

TEST(AutomationPng, SplitsLargeImagesIntoStoredBlocks)
{
    std::vector<uint8_t> pixels(640 * 480, 7);
    std::vector<PngColor> palette(256, PngColor{1, 2, 3});
    std::vector<uint8_t> png = EncodePngIndexed(pixels.data(), 640, 480, 640, palette);
    // raw = 480 * 641 bytes, stored in ceil(raw/65535) blocks of 5-byte headers
    size_t cbRaw = 480 * 641;
    size_t cblock = (cbRaw + 65534) / 65535;
    size_t cbIdat = 2 + cbRaw + cblock * 5 + 4;
    size_t cbExpected = 8 + (12 + 13) + (12 + 768) + (12 + cbIdat) + 12;
    EXPECT_EQ(png.size(), cbExpected);
}

// A camera 10 m back and 1.6 m up, looking slightly down the -Z axis
static CameraModel _CameraLookingDownZ(double degPitch)
{
    CameraModel camera;
    double r = degPitch * 3.14159265358979323846 / 180.0;
    camera.axisX = Vec3(1, 0, 0);
    // Pitched down: the view direction (-axisZ) points below the horizon
    camera.axisY = Vec3(0, std::cos(r), -std::sin(r));
    camera.axisZ = Vec3(0, std::sin(r), std::cos(r));
    camera.position = Vec3(0, 16, 100);
    camera.fovYDegrees = 50;
    camera.viewportWidth = 544;
    camera.viewportHeight = 306;
    return camera;
}

TEST(AutomationStage, StageAxesFollowCamera)
{
    CameraModel camera = _CameraLookingDownZ(10);
    StageFrame stage;
    stage.Init(camera, Vec3(0, 0, 50));

    // +x is screen right, +z is away from the camera, 1 unit = 10 world units
    Vec3 world = stage.ToWorld(Vec3(1, 0, 2));
    EXPECT_NEAR(world.x, 10, 1e-9);
    EXPECT_NEAR(world.y, 0, 1e-9);
    EXPECT_NEAR(world.z, 30, 1e-9);

    Vec3 back = stage.FromWorld(world);
    EXPECT_NEAR(back.x, 1, 1e-9);
    EXPECT_NEAR(back.z, 2, 1e-9);
}

TEST(AutomationStage, RotatedCameraKeepsScreenRight)
{
    // Camera turned 90 degrees: looking down -X, so screen right is -Z
    CameraModel camera;
    camera.axisX = Vec3(0, 0, -1);
    camera.axisY = Vec3(0, 1, 0);
    camera.axisZ = Vec3(1, 0, 0);
    camera.position = Vec3(100, 16, 0);
    StageFrame stage;
    stage.Init(camera, Vec3(50, 0, 0));

    Vec3 world = stage.ToWorld(Vec3(1, 0, 1));
    EXPECT_NEAR(world.x, 40, 1e-9);  // 1 m further from the camera
    EXPECT_NEAR(world.z, -10, 1e-9); // 1 m to screen right

    double xpLeft, xpRight, yp;
    bool fInView;
    ASSERT_TRUE(camera.FProject(stage.ToWorld(Vec3(-1, 0, 0)), &xpLeft, &yp, &fInView));
    ASSERT_TRUE(camera.FProject(stage.ToWorld(Vec3(1, 0, 0)), &xpRight, &yp, &fInView));
    EXPECT_LT(xpLeft, xpRight);
}

TEST(AutomationStage, ProjectionAndFloorRayAgree)
{
    CameraModel camera = _CameraLookingDownZ(12);
    for (double x : {-20.0, 0.0, 15.0})
    {
        for (double z : {20.0, 40.0, 60.0})
        {
            Vec3 floor(x, 0, z);
            double xp, yp;
            bool fInView;
            ASSERT_TRUE(camera.FProject(floor, &xp, &yp, &fInView));
            Vec3 hit;
            ASSERT_TRUE(camera.FFloorAt(xp, yp, &hit));
            EXPECT_NEAR(hit.x, x, 1e-6);
            EXPECT_NEAR(hit.z, z, 1e-6);
        }
    }

    // Straight ahead of the camera projects to the horizontal centre
    double xp, yp;
    bool fInView;
    ASSERT_TRUE(camera.FProject(Vec3(0, 0, 40), &xp, &yp, &fInView));
    EXPECT_NEAR(xp, 272, 1e-6);
    EXPECT_TRUE(fInView);

    // Behind the camera is not projectable
    EXPECT_FALSE(camera.FProject(Vec3(0, 0, 200), &xp, &yp, &fInView));
}

TEST(AutomationImage, DrawsTextLinesAndDownscales)
{
    Image8 img(40, 20, 0);
    int32_t dx = DrawLabel(&img, 2, 2, "X=1", 5, 9, 1);
    EXPECT_EQ(dx, DxText("X=1", 1));
    int32_t cFore = 0, cOutline = 0;
    for (uint8_t b : img.pixels)
    {
        cFore += b == 5;
        cOutline += b == 9;
    }
    EXPECT_GT(cFore, 5);
    EXPECT_GT(cOutline, cFore);

    Image8 imgLine(10, 10, 0);
    DrawLine(&imgLine, 0, 0, 9, 9, 3);
    for (int32_t i = 0; i < 10; i++)
        EXPECT_EQ(imgLine.Get(i, i), 3);
    // Off-image coordinates are clipped, not written
    DrawLine(&imgLine, -50, 5, 50, 5, 4);
    EXPECT_EQ(imgLine.Get(0, 5), 4);

    Image8 imgSmall = Downscale2x(imgLine);
    EXPECT_EQ(imgSmall.width, 5);
    EXPECT_EQ(imgSmall.Get(2, 2), 3);
}

TEST(AutomationImage, NearestPaletteColor)
{
    std::vector<PngColor> palette = {{0, 0, 0}, {250, 250, 250}, {0, 200, 220}, {255, 240, 0}};
    EXPECT_EQ(BNearestColor(palette, 255, 255, 255), 1);
    EXPECT_EQ(BNearestColor(palette, 10, 190, 210), 2);
    EXPECT_EQ(BNearestColor(palette, 250, 230, 20), 3);
}

TEST(AutomationImage, StageGridProjectsIntoView)
{
    CameraModel camera = _CameraLookingDownZ(12);
    StageFrame stage;
    stage.Init(camera, Vec3(0, 0, 50));
    Image8 img(544, 306, 0);
    DrawStageGrid(&img, camera, stage, -2, 2, -1, 3, {1, 2, 3, 4});
    int32_t cGrid = 0, cAxis = 0, cText = 0;
    for (uint8_t b : img.pixels)
    {
        cGrid += b == 1;
        cAxis += b == 2;
        cText += b == 3;
    }
    EXPECT_GT(cGrid, 200);
    EXPECT_GT(cAxis, 50);
    EXPECT_GT(cText, 20);
    // The x=0 axis passes through the screen centre column below the horizon
    double xp, yp;
    bool fInView;
    ASSERT_TRUE(camera.FProject(stage.ToWorld(Vec3(0, 0, 1)), &xp, &yp, &fInView));
    bool fNear = false;
    for (int32_t dx = -1; dx <= 1; dx++)
        fNear |= img.Get(static_cast<int32_t>(xp) + dx, static_cast<int32_t>(yp)) == 2;
    EXPECT_TRUE(fNear);
}

TEST(AutomationReview, CoveredFraction)
{
    ScreenBox back{0, 0, 100, 100};
    EXPECT_DOUBLE_EQ(DCoveredFraction(back, {50, 0, 150, 100}), 0.5);
    EXPECT_DOUBLE_EQ(DCoveredFraction(back, {-10, -10, 200, 200}), 1.0);
    EXPECT_DOUBLE_EQ(DCoveredFraction(back, {100, 0, 200, 100}), 0.0); // touching edges
    EXPECT_DOUBLE_EQ(DCoveredFraction({5, 5, 5, 9}, back), 0.0);       // empty box
}

TEST(AutomationReview, CutOffAtSides)
{
    EXPECT_FALSE(FCutOffAtSides({10, 10, 100, 100}, 544));
    EXPECT_TRUE(FCutOffAtSides({0, 10, 100, 100}, 544));   // touches the left edge
    EXPECT_TRUE(FCutOffAtSides({10, -5, 100, 100}, 544));  // head out of frame
    EXPECT_TRUE(FCutOffAtSides({500, 10, 544, 100}, 544)); // right edge
    EXPECT_FALSE(FCutOffAtSides({10, 10, 100, 400}, 544)); // legs cropped is fine
}

TEST(AutomationReview, ReadingTime)
{
    EXPECT_DOUBLE_EQ(DSecondsToRead(""), 2.0);
    EXPECT_DOUBLE_EQ(DSecondsToRead("THE END"), 2.0);
    EXPECT_DOUBLE_EQ(DSecondsToRead("one two three four five six seven eight nine"), 4.0);
    EXPECT_DOUBLE_EQ(DSecondsToRead("  spaced\n out\twords  here  and  there "), 3.0);
}

TEST(AutomationReview, GroupsFramesIntoRanges)
{
    auto vec = VecRanges({1, 4, 7, 20, 23, 40}, 3);
    ASSERT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0].from, 1);
    EXPECT_EQ(vec[0].to, 7);
    EXPECT_EQ(vec[1].from, 20);
    EXPECT_EQ(vec[1].to, 23);
    EXPECT_EQ(vec[2].from, 40);
    EXPECT_EQ(vec[2].to, 40);
    EXPECT_TRUE(VecRanges({}, 3).empty());
}

TEST(AutomationStage, YawAndFacingNames)
{
    EXPECT_NEAR(DegYaw(Vec3(0, 0, 1)), 0, 1e-9);
    EXPECT_NEAR(DegYaw(Vec3(1, 5, 0)), 90, 1e-9); // y ignored
    EXPECT_NEAR(DegWrap(270), -90, 1e-9);
    EXPECT_NEAR(DegWrap(-180), 180, 1e-9);
    EXPECT_NEAR(DegWrap(540), 180, 1e-9);
    EXPECT_STREQ(PszFacing(0), "camera");
    EXPECT_STREQ(PszFacing(20), "camera");
    EXPECT_STREQ(PszFacing(40), "front-right");
    EXPECT_STREQ(PszFacing(90), "right");
    EXPECT_STREQ(PszFacing(-90), "left");
    EXPECT_STREQ(PszFacing(179), "away");
    EXPECT_STREQ(PszFacing(-170), "away");
    EXPECT_STREQ(PszFacing(-45), "front-left");
}

TEST(AutomationStage, GroupSpots)
{
    auto two = VecGroupSpots(2, 1.0, true);
    ASSERT_EQ(two.size(), 2u);
    EXPECT_NEAR(two[0].x, -0.5, 1e-9);
    EXPECT_NEAR(two[1].x, 0.5, 1e-9);
    EXPECT_NEAR(two[0].z, 0, 1e-9);

    auto three = VecGroupSpots(3, 1.0, true);
    EXPECT_NEAR(three[1].x, 0, 1e-9);
    EXPECT_GT(three[1].z, three[0].z); // the middle stands further back
    EXPECT_NEAR(three[0].z, three[2].z, 1e-9);

    auto line = VecGroupSpots(4, 0.8, false);
    EXPECT_NEAR(line[3].x - line[0].x, 2.4, 1e-9);
    for (const GroupSpot &spot : line)
        EXPECT_NEAR(spot.z, 0, 1e-9);

    EXPECT_EQ(VecGroupSpots(1, 1.0, true).size(), 1u);
    EXPECT_NEAR(VecGroupSpots(1, 1.0, true)[0].x, 0, 1e-9);
    EXPECT_LT(SpotGroupFocus(1.0).z, 0); // toward the camera
}

static std::vector<Obstacle> NoObstacles(int32_t)
{
    return {};
}

TEST(AutomationPhysics, DropBouncesLowerEachTimeAndRests)
{
    PhysicsParams params;
    auto frames = VecSimulate(params, Vec3(0, 2, 0), Vec3(0, 0, 0), NoObstacles);
    // Falls 2 m in sqrt(2h/g) = 0.64 s: the impact lands in frame 4 at 6 fps
    int32_t ifrmFirstImpact = -1;
    for (size_t i = 0; i < frames.size() && ifrmFirstImpact < 0; i++)
    {
        if (frames[i].fImpact)
            ifrmFirstImpact = static_cast<int32_t>(i);
    }
    EXPECT_EQ(ifrmFirstImpact, 4);
    // The next peak is about restitution^2 of the drop height
    double yPeak = 0;
    for (size_t i = ifrmFirstImpact + 1; i < frames.size() && !frames[i].fImpact; i++)
        yPeak = std::max(yPeak, frames[i].position.y);
    EXPECT_NEAR(yPeak, 2 * 0.6 * 0.6, 0.15);
    // Comes to rest on the floor well before the time limit
    EXPECT_LT(frames.size(), 60u);
    EXPECT_NEAR(frames.back().position.y, 0, 1e-9);
    EXPECT_NEAR(frames.back().velocity.Length(), 0, 1e-9);
}

TEST(AutomationPhysics, DeadDropDoesNotBounce)
{
    PhysicsParams params;
    params.restitution = 0;
    auto frames = VecSimulate(params, Vec3(0, 1, 0), Vec3(0, 0, 0), NoObstacles);
    bool fLanded = false;
    for (const PhysicsFrame &frame : frames)
    {
        if (fLanded)
            EXPECT_NEAR(frame.position.y, 0, 1e-9);
        fLanded = fLanded || frame.fImpact;
    }
    EXPECT_TRUE(fLanded);
}

TEST(AutomationPhysics, FrictionStopsASlide)
{
    PhysicsParams params; // mu 0.4: 2 m/s stops after v^2 / (2 mu g) = 0.51 m
    auto frames = VecSimulate(params, Vec3(0, 0, 0), Vec3(2, 0, 0), NoObstacles);
    EXPECT_NEAR(frames.back().position.x, 0.51, 0.05);
    EXPECT_NEAR(frames.back().position.y, 0, 1e-9);
}

TEST(AutomationPhysics, ThrowLandsOnTarget)
{
    PhysicsParams params;
    params.airDrag = 0;
    Vec3 from(-2, 0, 1), to(1.5, 0, 3);
    Vec3 vel = VelocityToLand(from, to, 1.5, params.gravity);
    auto frames = VecSimulate(params, from, vel, NoObstacles);
    const PhysicsFrame *pfirst = nullptr;
    for (const PhysicsFrame &frame : frames)
    {
        if (frame.fImpact)
        {
            pfirst = &frame;
            break;
        }
    }
    ASSERT_NE(pfirst, nullptr);
    // Lands within one frame's travel of the target
    double dPerFrame = std::sqrt(vel.x * vel.x + vel.z * vel.z) / params.framesPerSecond;
    EXPECT_LT(std::hypot(pfirst->position.x - to.x, pfirst->position.z - to.z), dPerFrame);
}

TEST(AutomationPhysics, BouncesOffAnotherActor)
{
    PhysicsParams params;
    params.friction = 0;
    auto obstacles = [](int32_t) { return std::vector<Obstacle>{{1.5, 0, 0.3, 1.8}}; };
    auto frames = VecSimulate(params, Vec3(0, 0, 0), Vec3(3, 0, 0), obstacles);
    int32_t ifrmHit = -1;
    for (size_t i = 0; i < frames.size(); i++)
    {
        if (frames[i].fImpact && frames[i].iobstacleHit == 0)
        {
            ifrmHit = static_cast<int32_t>(i);
            break;
        }
    }
    ASSERT_GE(ifrmHit, 0);
    EXPECT_LT(frames[ifrmHit].velocity.x, 0); // heading back
    for (const PhysicsFrame &frame : frames)
        EXPECT_LE(frame.position.x, 1.5 - 0.3 - 0.25 + 1e-6); // never inside it
}

// Closest any leg of a walk comes to a point
static double DistClosest(const Vec3 &start, const std::vector<RouteStep> &steps, double x, double z)
{
    double dBest = 1e9;
    Vec3 a = start;
    for (const RouteStep &step : steps)
    {
        Vec3 b = step.position;
        for (int32_t i = 0; i <= 100; i++)
        {
            double t = i / 100.0;
            dBest = std::min(dBest, std::hypot(a.x + (b.x - a.x) * t - x, a.z + (b.z - a.z) * t - z));
        }
        a = b;
    }
    return dBest;
}

TEST(AutomationPhysics, WalkGoesAroundSomeoneInTheWay)
{
    std::vector<Obstacle> obstacles{{0, 0, 0.3, 1.8}};
    auto steps = VecRouteAround(Vec3(-3, 0, 0), {Vec3(3, 0, 0)}, obstacles, 0.25);
    ASSERT_GE(steps.size(), 2u);
    EXPECT_EQ(steps.back().itarget, 0); // still ends where asked
    EXPECT_EQ(steps.front().itarget, -1);
    EXPECT_GE(DistClosest(Vec3(-3, 0, 0), steps, 0, 0), 0.3 + 0.25);
    EXPECT_LT(steps.front().position.z, 0); // passes in front of them, toward the camera
}

TEST(AutomationPhysics, WalkLeavesClearPathsAndDestinationsAlone)
{
    std::vector<Obstacle> obstacles{{0, 2, 0.3, 1.8}, {3, 0, 0.3, 1.8}};
    // Passes well clear of the first, and walks up to the second on purpose
    auto steps = VecRouteAround(Vec3(-3, 0, 0), {Vec3(3, 0, 0)}, obstacles, 0.25);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].itarget, 0);
}

TEST(AutomationPhysics, WalkThreadsThroughACrowd)
{
    std::vector<Obstacle> obstacles{{-1, 0.1, 0.3, 1.8}, {0, -0.1, 0.3, 1.8}, {1, 0.05, 0.3, 1.8}};
    auto steps = VecRouteAround(Vec3(-3, 0, 0), {Vec3(3, 0, 0)}, obstacles, 0.25);
    for (const Obstacle &obs : obstacles)
        EXPECT_GE(DistClosest(Vec3(-3, 0, 0), steps, obs.x, obs.z), obs.radius + 0.25) << obs.x;
    EXPECT_EQ(steps.back().itarget, 0);
}

TEST(AutomationDirecting, CutsOnlyWhenItPaysOff)
{
    // Two cameras; camera 1 is slightly better on beat 1 only: not worth a cut
    std::vector<std::vector<double>> score{{2, 1}, {1.9, 2.0}, {2, 1}};
    auto cams = VecChooseShots(score, 0.5);
    EXPECT_EQ(cams, (std::vector<int32_t>{0, 0, 0}));
    // Much better: cut to it and back
    score[1] = {0, 3};
    cams = VecChooseShots(score, 0.5);
    EXPECT_EQ(cams, (std::vector<int32_t>{0, 1, 0}));
}

TEST(AutomationDirecting, ShotReverseShot)
{
    // A speaks, B speaks, A speaks: each is best on their own camera
    std::vector<std::vector<double>> score{{3, 0.5}, {0.5, 3}, {3, 0.5}, {0.5, 3}};
    EXPECT_EQ(VecChooseShots(score, 0.6), (std::vector<int32_t>{0, 1, 0, 1}));
    EXPECT_TRUE(VecChooseShots({}, 0.6).empty());
}

TEST(AutomationDirecting, ShortShotsMergeIntoNeighbours)
{
    // Beats at 1, 20, 23, 40; the 3-frame shot on camera 2 is too short
    std::vector<int32_t> cams{0, 2, 1, 1};
    HoldShortShots({1, 20, 23, 40}, 60, 9, &cams);
    EXPECT_EQ(cams, (std::vector<int32_t>{0, 0, 1, 1}));
    // A short first shot takes the next shot's camera
    cams = {3, 0, 0};
    HoldShortShots({1, 4, 30}, 50, 9, &cams);
    EXPECT_EQ(cams, (std::vector<int32_t>{0, 0, 0}));
    // Long enough shots stay as they are
    cams = {0, 1, 0};
    HoldShortShots({1, 20, 40}, 60, 9, &cams);
    EXPECT_EQ(cams, (std::vector<int32_t>{0, 1, 0}));
    // Given the scores, a short shot goes to whichever neighbour's camera
    // shows it better (an actor running out of the close-up keeps the wide)
    cams = {0, 1, 2, 0};
    std::vector<std::vector<double>> score{{5, 1, 1}, {1, 5, 4}, {1, 1, 5}, {5, 1, 1}};
    std::vector<int32_t> camsUnscored = cams;
    HoldShortShots({1, 20, 24, 40}, 60, 9, &camsUnscored);
    EXPECT_EQ(camsUnscored, (std::vector<int32_t>{0, 0, 2, 0}));
    HoldShortShots({1, 20, 24, 40}, 60, 9, &cams, &score);
    EXPECT_EQ(cams, (std::vector<int32_t>{0, 2, 2, 0}));
}

TEST(AutomationDirecting, FramingPrefersHalfTheFrame)
{
    EXPECT_NEAR(DFramingScore(0.5), 1, 1e-9);
    EXPECT_LT(DFramingScore(0.1), DFramingScore(0.4));
    EXPECT_LT(DFramingScore(1.0), DFramingScore(0.6));
    EXPECT_GE(DFramingScore(1.2), 0);
    EXPECT_EQ(DFramingScore(0), 0);
    // A close single aims taller: legs cropped below the frame are fine
    EXPECT_NEAR(DFramingScore(0.8, 0.8), 1, 1e-9);
    EXPECT_GT(DFramingScore(0.75, 0.8), DFramingScore(0.45, 0.8));
    // A face filling the frame is worse than a slightly small figure
    EXPECT_LT(DFramingScore(1.3, 0.8), DFramingScore(0.5, 0.8));
}

TEST(AutomationDirecting, NoJumpCutsBetweenTheSameView)
{
    // Cameras 0 and 1 film the same view (a backdrop swap); 2 is a real
    // other angle that scores a little lower
    std::vector<std::vector<double>> score{{7, 7.2, 6.5}, {7.2, 7, 6.5}, {7, 7.2, 6.5}, {7.2, 7, 6.5}};
    std::vector<int32_t> starts{1, 30, 60, 90};
    std::vector<bool> change{false, true, true, true};
    ShotPacing pacing{0.4, 0.25, 36, 0.05, 0.75};
    auto unaware = VecChooseShotsPaced(score, starts, 120, change, pacing);
    EXPECT_NE(std::find(unaware.begin() + 1, unaware.end(), unaware[0] == 0 ? 1 : 0), unaware.end())
        << "without knowing, it swaps between the twins";
    std::vector<std::vector<bool>> jump{{false, true, false}, {true, false, false}, {false, false, false}};
    pacing.dJumpPenalty = 3;
    auto cams = VecChooseShotsPaced(score, starts, 120, change, pacing, nullptr, &jump);
    for (size_t i = 1; i < cams.size(); i++)
        EXPECT_FALSE(jump[cams[i - 1]][cams[i]]) << "cut " << cams[i - 1] << " -> " << cams[i];
}

TEST(AutomationDirecting, SameViewMeansCloseAndAimedAlike)
{
    // Positions in metres, directions as unit vectors on the floor
    EXPECT_TRUE(FSameView(0, 0, 0, 1, 0.3, 0.2, 0.1, 0.995, 1.0));
    EXPECT_FALSE(FSameView(0, 0, 0, 1, 3, 0, 0, 1, 1.0)) << "moved sideways";
    EXPECT_FALSE(FSameView(0, 0, 0, 1, 0, 0, 0.5, 0.866, 1.0)) << "turned 30 degrees";
}

TEST(AutomationDirecting, CamerasStayOnOneSideOfTheLine)
{
    // A at (0, 0) and B at (2, 0); the opening camera films them from z < 0
    EXPECT_TRUE(FSameSideOfLine(0, 0, 2, 0, 1, -5, 3, -1));
    EXPECT_FALSE(FSameSideOfLine(0, 0, 2, 0, 1, -5, 1, 5)) << "the reverse side swaps who is on the left";
    EXPECT_FALSE(FSameSideOfLine(2, 0, 0, 0, 1, -5, -1, 3)) << "whichever of the pair speaks";
    // A camera (nearly) along the line, or an opening camera on it, crosses nothing
    EXPECT_TRUE(FSameSideOfLine(0, 0, 2, 0, 1, -5, 6, 0.2));
    EXPECT_TRUE(FSameSideOfLine(0, 0, 2, 0, -6, 0.1, 1, 5));
    // People standing on one spot have no line
    EXPECT_TRUE(FSameSideOfLine(0, 0, 0, 0, 1, -5, 1, 5));
}

TEST(AutomationDirecting, PacedEditingCutsOnDialogueAndAvoidsLongHolds)
{
    // Two speakers alternating; the wide shot (0) is slightly better for both
    // than their singles (1 for A, 2 for B). Unpaced, the wide shot holds.
    std::vector<std::vector<double>> score{{7.4, 7.2, 4}, {7.3, 4, 6.7}, {7.4, 7.2, 4}, {7.3, 4, 6.7}};
    std::vector<int32_t> starts{1, 30, 60, 90};
    std::vector<bool> speakerChange{false, true, true, true};
    EXPECT_EQ(VecChooseShots(score, 0.4), (std::vector<int32_t>{0, 0, 0, 0}));
    // Paced: cuts on speaker changes are cheap, and holding the wide shot
    // past 6 s (36 frames) costs, so it goes to the singles
    ShotPacing pacing{0.4, 0.25, 36, 0.05, 0.75};
    auto cams = VecChooseShotsPaced(score, starts, 120, speakerChange, pacing);
    int ccut = 0;
    for (size_t i = 1; i < cams.size(); i++)
        ccut += cams[i] != cams[i - 1];
    EXPECT_GE(ccut, 2) << "a 20 s exchange shouldn't be one static shot";
    // A short exchange stays on the best shot: no hold cost yet
    auto brief = VecChooseShotsPaced({{7.4, 7.2, 4}, {7.3, 4, 6.7}}, {1, 12}, 24, {false, true}, pacing);
    EXPECT_EQ(brief, (std::vector<int32_t>{0, 0}));
    // A clearly bad angle is never chosen just to add variety
    auto noBad = VecChooseShotsPaced({{7, -3}, {7, -3}, {7, -3}}, {1, 40, 80}, 120, {false, false, false}, pacing);
    EXPECT_EQ(noBad, (std::vector<int32_t>{0, 0, 0}));
    // Nor a merely worse one (0.8 below the best): a long hold beats it
    auto worse = VecChooseShotsPaced({{6.1, 5.3}, {7.0, 3.9}, {6.1, 5.3}, {7.0, 3.9}}, {1, 30, 60, 90}, 120,
                                     {false, true, true, true}, pacing);
    EXPECT_EQ(worse, (std::vector<int32_t>{0, 0, 0, 0}));
    // ...unless it is a clean single (the speaker's face large and turned to
    // us), which may trail the two-shot further: then a long exchange cuts
    std::vector<std::vector<double>> twoShot{{7.3, 6.5, 4}, {7.4, 4, 5.7}, {7.3, 6.5, 4}, {7.4, 4, 5.7}};
    std::vector<std::vector<bool>> readable{
        {false, true, false}, {false, false, true}, {false, true, false}, {false, false, true}};
    ShotPacing pacingSingles{0.4, 0.25, 36, 0.05, 0.75, 1.75};
    auto held = VecChooseShotsPaced(twoShot, starts, 120, speakerChange, pacingSingles);
    EXPECT_EQ(held, (std::vector<int32_t>{0, 0, 0, 0})) << "without knowing which shots are singles";
    auto cut = VecChooseShotsPaced(twoShot, starts, 120, speakerChange, pacingSingles, &readable);
    ccut = 0;
    for (size_t i = 1; i < cut.size(); i++)
        ccut += cut[i] != cut[i - 1];
    EXPECT_GE(ccut, 1) << "a 20 s exchange with clean singles available";
    // A bad shot stays out even if marked readable
    std::vector<std::vector<bool>> readableBad{{false, true}, {false, true}, {false, true}};
    auto stillNoBad = VecChooseShotsPaced({{7, 4}, {7, 4}, {7, 4}}, {1, 40, 80}, 120, {false, false, false},
                                          pacingSingles, &readableBad);
    EXPECT_EQ(stillNoBad, (std::vector<int32_t>{0, 0, 0}));
}
