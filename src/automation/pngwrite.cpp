/***************************************************************************
    Automation bridge: minimal PNG encoder.
***************************************************************************/
#include "pngwrite.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace automation
{

static uint32_t _Crc32(const uint8_t *pb, size_t cb, uint32_t crc = 0)
{
    static uint32_t rgcrc[256];
    static bool fInit = false;
    if (!fInit)
    {
        for (uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            rgcrc[n] = c;
        }
        fInit = true;
    }

    crc = ~crc;
    for (size_t ib = 0; ib < cb; ib++)
        crc = rgcrc[(crc ^ pb[ib]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void _PutU32(std::vector<uint8_t> *pv, uint32_t lw)
{
    pv->push_back(static_cast<uint8_t>(lw >> 24));
    pv->push_back(static_cast<uint8_t>(lw >> 16));
    pv->push_back(static_cast<uint8_t>(lw >> 8));
    pv->push_back(static_cast<uint8_t>(lw));
}

static void _PutChunk(std::vector<uint8_t> *pvOut, const char *pszType, const std::vector<uint8_t> &data)
{
    _PutU32(pvOut, static_cast<uint32_t>(data.size()));
    size_t ibType = pvOut->size();
    pvOut->insert(pvOut->end(), pszType, pszType + 4);
    pvOut->insert(pvOut->end(), data.begin(), data.end());
    _PutU32(pvOut, _Crc32(pvOut->data() + ibType, 4 + data.size()));
}

std::vector<uint8_t> EncodePngIndexed(const uint8_t *pixels, int32_t width, int32_t height, int32_t stride,
                                      const std::vector<PngColor> &palette)
{
    std::vector<uint8_t> out;
    static const uint8_t rgbSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), rgbSignature, rgbSignature + 8);

    // IHDR: width, height, bit depth 8, color type 3 (indexed)
    std::vector<uint8_t> ihdr;
    _PutU32(&ihdr, static_cast<uint32_t>(width));
    _PutU32(&ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8);
    ihdr.push_back(3);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    _PutChunk(&out, "IHDR", ihdr);

    std::vector<uint8_t> plte;
    size_t ccolor = palette.size() > 256 ? 256 : palette.size();
    for (size_t icolor = 0; icolor < ccolor; icolor++)
    {
        plte.push_back(palette[icolor].red);
        plte.push_back(palette[icolor].green);
        plte.push_back(palette[icolor].blue);
    }
    _PutChunk(&out, "PLTE", plte);

    // Raw scanlines, each prefixed with filter type 0
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (width + 1));
    for (int32_t y = 0; y < height; y++)
    {
        raw.push_back(0);
        const uint8_t *prow = pixels + static_cast<size_t>(y) * stride;
        raw.insert(raw.end(), prow, prow + width);
    }

    // zlib stream of stored blocks (max 65535 bytes each) + Adler-32
    std::vector<uint8_t> idat;
    idat.push_back(0x78);
    idat.push_back(0x01);
    size_t ib = 0;
    do
    {
        size_t cb = raw.size() - ib;
        if (cb > 65535)
            cb = 65535;
        bool fLast = ib + cb == raw.size();
        idat.push_back(fLast ? 1 : 0);
        idat.push_back(static_cast<uint8_t>(cb));
        idat.push_back(static_cast<uint8_t>(cb >> 8));
        idat.push_back(static_cast<uint8_t>(~cb));
        idat.push_back(static_cast<uint8_t>(~cb >> 8));
        idat.insert(idat.end(), raw.begin() + ib, raw.begin() + ib + cb);
        ib += cb;
    } while (ib < raw.size());

    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    _PutU32(&idat, (b << 16) | a);
    _PutChunk(&out, "IDAT", idat);
    _PutChunk(&out, "IEND", std::vector<uint8_t>());
    return out;
}

bool FWriteFileBytes(const std::string &strPath, const std::vector<uint8_t> &bytes, std::string *pstrError)
{
    FILE *pfile = fopen(strPath.c_str(), "wb");
    if (pfile == nullptr)
    {
        *pstrError = "cannot create '" + strPath + "': " + strerror(errno);
        return false;
    }
    size_t cbWritten = fwrite(bytes.data(), 1, bytes.size(), pfile);
    bool fOk = cbWritten == bytes.size();
    if (fclose(pfile) != 0)
        fOk = false;
    if (!fOk)
        *pstrError = "failed writing '" + strPath + "'";
    return fOk;
}

} // namespace automation
