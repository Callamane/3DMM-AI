// Unit tests for the engine-independent parts of the automation bridge.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "json.h"
#include "pngwrite.h"
#include "stage.h"

using namespace automation;

TEST(AutomationJson, ParsesRequest)
{
    Json json;
    std::string strError;
    ASSERT_TRUE(Json::FParse(R"({"id": 7, "cmd": "add_actor", "args": {"template": "Ron", "position": {"x": -1.5, "z": 2}}})",
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
    ASSERT_TRUE(Json::FParse(R"(["a\"b\\c\n", "caf\u00e9", "\ud83c\udfac", true, false, null, 1e3, -0.25])", &json,
                             &strError))
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
    for (const char *psz : {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "[1] 2",
                            "\"\\ud800\"", "{1:2}"})
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
    EXPECT_NEAR(world.x, 40, 1e-9); // 1 m further from the camera
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
