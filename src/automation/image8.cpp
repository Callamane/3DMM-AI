/***************************************************************************
    Automation bridge: 8-bit image helpers (see image8.h).
***************************************************************************/
#include "image8.h"

#include <cmath>
#include <cstdlib>

namespace automation
{

uint8_t BNearestColor(const std::vector<PngColor> &palette, uint8_t r, uint8_t g, uint8_t b)
{
    int32_t lwBest = 0x7FFFFFFF;
    uint8_t bBest = 0;
    for (size_t i = 0; i < palette.size() && i < 256; i++)
    {
        int32_t dr = palette[i].red - r, dg = palette[i].green - g, db = palette[i].blue - b;
        int32_t lw = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (lw < lwBest)
        {
            lwBest = lw;
            bBest = static_cast<uint8_t>(i);
        }
    }
    return bBest;
}

void DrawLine(Image8 *pimg, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t b)
{
    int32_t dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int32_t cstep = 0; cstep < 20000; cstep++)
    {
        pimg->Set(x0, y0, b);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void FillRect(Image8 *pimg, int32_t x, int32_t y, int32_t dx, int32_t dy, uint8_t b)
{
    for (int32_t yy = y; yy < y + dy; yy++)
    {
        for (int32_t xx = x; xx < x + dx; xx++)
            pimg->Set(xx, yy, b);
    }
}

/***************************************************************************
    3x5 font: each glyph is 5 rows of 3 bits (MSB = left column)
***************************************************************************/
struct Glyph
{
    char ch;
    uint8_t rgrow[5];
};

static const Glyph _rgglyph[] = {
    {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}},  {'3', {7, 1, 7, 1, 7}},
    {'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}},  {'7', {7, 1, 2, 2, 2}},
    {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}}, {'A', {2, 5, 7, 5, 5}},  {'B', {6, 5, 6, 5, 6}},
    {'C', {3, 4, 4, 4, 3}}, {'D', {6, 5, 5, 5, 6}}, {'E', {7, 4, 6, 4, 7}},  {'F', {7, 4, 6, 4, 4}},
    {'G', {3, 4, 5, 5, 3}}, {'H', {5, 5, 7, 5, 5}}, {'I', {7, 2, 2, 2, 7}},  {'J', {1, 1, 1, 5, 2}},
    {'K', {5, 5, 6, 5, 5}}, {'L', {4, 4, 4, 4, 7}}, {'M', {5, 7, 7, 5, 5}},  {'N', {6, 5, 5, 5, 5}},
    {'O', {2, 5, 5, 5, 2}}, {'P', {6, 5, 6, 4, 4}}, {'Q', {2, 5, 5, 6, 3}},  {'R', {6, 5, 6, 5, 5}},
    {'S', {3, 4, 2, 1, 6}}, {'T', {7, 2, 2, 2, 2}}, {'U', {5, 5, 5, 5, 7}},  {'V', {5, 5, 5, 5, 2}},
    {'W', {5, 5, 7, 7, 5}}, {'X', {5, 5, 2, 5, 5}}, {'Y', {5, 5, 2, 2, 2}},  {'Z', {7, 1, 2, 4, 7}},
    {'-', {0, 0, 7, 0, 0}}, {'.', {0, 0, 0, 0, 2}}, {'#', {5, 7, 5, 7, 5}},  {'=', {0, 7, 0, 7, 0}},
    {':', {0, 2, 0, 2, 0}}, {'+', {0, 2, 7, 2, 0}}, {',', {0, 0, 0, 2, 4}},  {'/', {1, 1, 2, 4, 4}},
    {'(', {1, 2, 2, 2, 1}}, {')', {4, 2, 2, 2, 4}}, {'\'', {2, 2, 0, 0, 0}}, {'!', {2, 2, 2, 0, 2}},
};

static const Glyph *_PglyphFor(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch = static_cast<char>(ch - 'a' + 'A');
    for (const Glyph &glyph : _rgglyph)
    {
        if (glyph.ch == ch)
            return &glyph;
    }
    return nullptr;
}

int32_t DxText(const std::string &str, int32_t scale)
{
    if (str.empty())
        return 0;
    return static_cast<int32_t>(str.size()) * 4 * scale - scale;
}

static void _DrawGlyphs(Image8 *pimg, int32_t x, int32_t y, const std::string &str, uint8_t b, int32_t scale)
{
    for (size_t ich = 0; ich < str.size(); ich++)
    {
        const Glyph *pglyph = _PglyphFor(str[ich]);
        if (pglyph == nullptr)
            continue;
        int32_t xGlyph = x + static_cast<int32_t>(ich) * 4 * scale;
        for (int32_t row = 0; row < 5; row++)
        {
            for (int32_t col = 0; col < 3; col++)
            {
                if (pglyph->rgrow[row] & (4 >> col))
                    FillRect(pimg, xGlyph + col * scale, y + row * scale, scale, scale, b);
            }
        }
    }
}

int32_t DrawLabel(Image8 *pimg, int32_t x, int32_t y, const std::string &str, uint8_t bFore, uint8_t bOutline,
                  int32_t scale)
{
    for (int32_t dy = -1; dy <= 1; dy++)
    {
        for (int32_t dx = -1; dx <= 1; dx++)
        {
            if (dx != 0 || dy != 0)
                _DrawGlyphs(pimg, x + dx, y + dy, str, bOutline, scale);
        }
    }
    _DrawGlyphs(pimg, x, y, str, bFore, scale);
    return DxText(str, scale);
}

Image8 Downscale2x(const Image8 &img)
{
    Image8 imgSmall(img.width / 2, img.height / 2);
    for (int32_t y = 0; y < imgSmall.height; y++)
    {
        for (int32_t x = 0; x < imgSmall.width; x++)
            imgSmall.pixels[static_cast<size_t>(y) * imgSmall.width + x] = img.Get(x * 2, y * 2);
    }
    return imgSmall;
}

void Blit(Image8 *pdst, const Image8 &src, int32_t x, int32_t y)
{
    for (int32_t yy = 0; yy < src.height; yy++)
    {
        for (int32_t xx = 0; xx < src.width; xx++)
            pdst->Set(x + xx, y + yy, src.Get(xx, yy));
    }
}

/***************************************************************************
    Stage grid
***************************************************************************/
const double kdStep = 0.1;     // metres between samples along a grid line
const double kdPixelMax = 1e4; // ignore projections this far off screen

static bool _FProjectStage(const CameraModel &camera, const StageFrame &stage, double x, double z, int32_t *pxp,
                           int32_t *pyp, bool *pfInView)
{
    double xp, yp;
    if (!camera.FProject(stage.ToWorld(Vec3(x, 0, z)), &xp, &yp, pfInView))
        return false;
    if (std::fabs(xp) > kdPixelMax || std::fabs(yp) > kdPixelMax)
        return false;
    *pxp = static_cast<int32_t>(std::lround(xp));
    *pyp = static_cast<int32_t>(std::lround(yp));
    return true;
}

// Draws the polyline from (x0, z0) to (x1, z1) on the floor. Returns the
// first in-view pixel along it (for labelling) in *pxpLabel/*pypLabel.
static bool _FDrawFloorLine(Image8 *pimg, const CameraModel &camera, const StageFrame &stage, double x0, double z0,
                            double x1, double z1, uint8_t b, int32_t *pxpLabel, int32_t *pypLabel)
{
    double dLength = std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
    int32_t csample = static_cast<int32_t>(dLength / kdStep) + 1;
    bool fPrev = false, fLabel = false;
    int32_t xpPrev = 0, ypPrev = 0;
    for (int32_t i = 0; i <= csample; i++)
    {
        double t = static_cast<double>(i) / csample;
        int32_t xp, yp;
        bool fInView;
        bool fOk = _FProjectStage(camera, stage, x0 + (x1 - x0) * t, z0 + (z1 - z0) * t, &xp, &yp, &fInView);
        if (fOk && fPrev)
            DrawLine(pimg, xpPrev, ypPrev, xp, yp, b);
        if (fOk && fInView && !fLabel)
        {
            *pxpLabel = xp;
            *pypLabel = yp;
            fLabel = true;
        }
        fPrev = fOk;
        xpPrev = xp;
        ypPrev = yp;
    }
    return fLabel;
}

void DrawStageGrid(Image8 *pimg, const CameraModel &camera, const StageFrame &stage, int32_t xMin, int32_t xMax,
                   int32_t zMin, int32_t zMax, const OverlayColors &colors)
{
    struct Label
    {
        int32_t xp, yp;
        std::string str;
    };
    std::vector<Label> vecLabels;
    int32_t xp, yp;

    // Lines of constant x (running away from the camera)
    for (int32_t x = xMin; x <= xMax; x++)
    {
        uint8_t b = x == 0 ? colors.bAxis : colors.bGrid;
        if (_FDrawFloorLine(pimg, camera, stage, x, zMin, x, zMax, b, &xp, &yp))
            vecLabels.push_back({xp, yp, "X=" + std::to_string(x)});
    }
    // Lines of constant z (running across the screen)
    for (int32_t z = zMin; z <= zMax; z++)
    {
        uint8_t b = z == 0 ? colors.bAxis : colors.bGrid;
        if (_FDrawFloorLine(pimg, camera, stage, xMin, z, xMax, z, b, &xp, &yp))
            vecLabels.push_back({xp, yp, "Z=" + std::to_string(z)});
    }

    // Labels last so lines don't cross them; keep them inside the image and
    // skip any that would overlap one already drawn (far lines bunch up)
    struct Box
    {
        int32_t x, y, dx, dy;
    };
    std::vector<Box> vecPlaced;
    for (const Label &label : vecLabels)
    {
        int32_t dx = DxText(label.str, 1);
        int32_t xLabel = label.xp + 2;
        int32_t yLabel = label.yp - 7;
        if (xLabel + dx > pimg->width - 2)
            xLabel = pimg->width - 2 - dx;
        if (xLabel < 1)
            xLabel = 1;
        if (yLabel < 1)
            yLabel = 1;
        if (yLabel > pimg->height - 7)
            yLabel = pimg->height - 7;
        bool fOverlap = false;
        for (const Box &box : vecPlaced)
        {
            if (xLabel < box.x + box.dx + 3 && box.x < xLabel + dx + 3 && yLabel < box.y + box.dy + 2 &&
                box.y < yLabel + 7)
            {
                fOverlap = true;
            }
        }
        if (fOverlap)
            continue;
        vecPlaced.push_back({xLabel, yLabel, dx, 5});
        DrawLabel(pimg, xLabel, yLabel, label.str, colors.bText, colors.bOutline, 1);
    }
}

} // namespace automation
