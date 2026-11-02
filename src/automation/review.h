/***************************************************************************
    Automation bridge: the rules behind review_movie (see reviewcmd.cpp),
    kept engine independent so they can be unit tested.
***************************************************************************/
#ifndef AUTOMATION_REVIEW_H
#define AUTOMATION_REVIEW_H

#include <cstdint>
#include <string>
#include <vector>

namespace automation
{

struct ScreenBox
{
    int32_t left, top, right, bottom;

    int64_t Area() const
    {
        return right > left && bottom > top ? static_cast<int64_t>(right - left) * (bottom - top) : 0;
    }
};

// Fraction (0..1) of back's area that front covers
double DCoveredFraction(const ScreenBox &back, const ScreenBox &front);

// Whether box runs off the left, right or top of a view dxp pixels wide.
// Running off the bottom is left out: close shots often crop at the legs.
bool FCutOffAtSides(const ScreenBox &box, int32_t dxp);

// Seconds a viewer needs to read text on screen: a second to notice it plus
// three words a second, at least two seconds
double DSecondsToRead(const std::string &text);

struct FrameRange
{
    int32_t from, to;
};

// Groups sorted frame numbers into ranges, joining frames at most cfrmGap
// apart (so samples taken every few frames make one range)
std::vector<FrameRange> VecRanges(const std::vector<int32_t> &vecFrames, int32_t cfrmGap);

} // namespace automation

#endif // AUTOMATION_REVIEW_H
