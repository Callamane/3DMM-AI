/***************************************************************************
    Automation bridge: review_movie rules (see review.h).
***************************************************************************/
#include "review.h"

#include <algorithm>
#include <sstream>

namespace automation
{

double DCoveredFraction(const ScreenBox &back, const ScreenBox &front)
{
    int64_t area = back.Area();
    if (area == 0)
        return 0;
    ScreenBox overlap{std::max(back.left, front.left), std::max(back.top, front.top), std::min(back.right, front.right),
                      std::min(back.bottom, front.bottom)};
    return static_cast<double>(overlap.Area()) / area;
}

bool FCutOffAtSides(const ScreenBox &box, int32_t dxp)
{
    return box.left <= 0 || box.top <= 0 || box.right >= dxp;
}

double DSecondsToRead(const std::string &text)
{
    std::istringstream stream(text);
    std::string word;
    int32_t cword = 0;
    while (stream >> word)
        cword++;
    return std::max(2.0, 1.0 + cword / 3.0);
}

std::vector<FrameRange> VecRanges(const std::vector<int32_t> &vecFrames, int32_t cfrmGap)
{
    std::vector<FrameRange> vecRanges;
    for (int32_t nfrm : vecFrames)
    {
        if (!vecRanges.empty() && nfrm - vecRanges.back().to <= cfrmGap)
            vecRanges.back().to = nfrm;
        else
            vecRanges.push_back({nfrm, nfrm});
    }
    return vecRanges;
}

} // namespace automation
