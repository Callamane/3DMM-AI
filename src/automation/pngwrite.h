/***************************************************************************
    Automation bridge: minimal PNG encoder for 8-bit palettized images.

    Uses zlib "stored" (uncompressed) deflate blocks so there is no zlib
    dependency. A 640x480 frame is ~300 KB, which is fine for previews.
***************************************************************************/
#ifndef AUTOMATION_PNGWRITE_H
#define AUTOMATION_PNGWRITE_H

#include <cstdint>
#include <string>
#include <vector>

namespace automation
{

struct PngColor
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

// Encodes an 8-bit indexed image. pixels has height rows of stride bytes
// (stride >= width). Returns the PNG bytes.
std::vector<uint8_t> EncodePngIndexed(const uint8_t *pixels, int32_t width, int32_t height, int32_t stride,
                                      const std::vector<PngColor> &palette);

// Writes bytes to a file. Returns false and sets *pstrError on failure.
bool FWriteFileBytes(const std::string &strPath, const std::vector<uint8_t> &bytes, std::string *pstrError);

} // namespace automation

#endif // AUTOMATION_PNGWRITE_H
