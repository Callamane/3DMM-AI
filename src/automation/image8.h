/***************************************************************************
    Automation bridge: small 8-bit (palettized) image helpers used to
    annotate renders for AI clients: lines, a tiny bitmap font, downscaling,
    tiling into contact sheets, and the projected stage-coordinate grid.

    Engine independent (standard library only) so it can be unit tested.
***************************************************************************/
#ifndef AUTOMATION_IMAGE8_H
#define AUTOMATION_IMAGE8_H

#include <cstdint>
#include <string>
#include <vector>

#include "pngwrite.h"
#include "stage.h"

namespace automation
{

struct Image8
{
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> pixels; // width * height palette indices

    Image8() = default;
    Image8(int32_t dxp, int32_t dyp, uint8_t bFill = 0) : width(dxp), height(dyp), pixels(dxp * dyp, bFill)
    {
    }
    void Set(int32_t x, int32_t y, uint8_t b)
    {
        if (x >= 0 && y >= 0 && x < width && y < height)
            pixels[static_cast<size_t>(y) * width + x] = b;
    }
    uint8_t Get(int32_t x, int32_t y) const
    {
        return pixels[static_cast<size_t>(y) * width + x];
    }
};

// Index of the palette entry closest to (r, g, b)
uint8_t BNearestColor(const std::vector<PngColor> &palette, uint8_t r, uint8_t g, uint8_t b);

void DrawLine(Image8 *pimg, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t b);
void FillRect(Image8 *pimg, int32_t x, int32_t y, int32_t dx, int32_t dy, uint8_t b);

// Draws text with the built-in 3x5 font (digits, A-Z, a few symbols; other
// characters render as blanks), scaled by `scale`, with a 1-pixel outline
// in bOutline for legibility over any background. Returns the text width.
int32_t DrawText(Image8 *pimg, int32_t x, int32_t y, const std::string &str, uint8_t bFore, uint8_t bOutline,
                 int32_t scale = 2);
int32_t DxText(const std::string &str, int32_t scale = 2);

// Halves the size by averaging is impossible with a palette, so this keeps
// one pixel of each 2x2 block.
Image8 Downscale2x(const Image8 &img);

// Copies src into dst at (x, y), clipped.
void Blit(Image8 *pdst, const Image8 &src, int32_t x, int32_t y);

struct OverlayColors
{
    uint8_t bGrid;
    uint8_t bAxis;
    uint8_t bText;
    uint8_t bOutline;
};

// Draws the floor grid of stage coordinates (1 m lines for x in [xMin, xMax]
// and z in [zMin, zMax]) projected through the camera, labelling x values
// along the nearest visible z line and z values along the left edge.
void DrawStageGrid(Image8 *pimg, const CameraModel &camera, const StageFrame &stage, int32_t xMin, int32_t xMax,
                   int32_t zMin, int32_t zMax, const OverlayColors &colors);

} // namespace automation

#endif // AUTOMATION_IMAGE8_H
