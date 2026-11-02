/***************************************************************************
    Automation bridge: choosing shots for auto_camera (see directcmd.cpp),
    kept engine independent so it can be unit tested.

    A scene is split into beats (each time someone starts to speak). Every
    camera gets a score for every beat; the shot list is the sequence of
    cameras with the best total score after paying a cost for each cut, so
    the camera only changes when the new angle is clearly better. Shots that
    would be too short to register are then merged into a neighbour.
***************************************************************************/
#ifndef AUTOMATION_DIRECTING_H
#define AUTOMATION_DIRECTING_H

#include <cstdint>
#include <vector>

namespace automation
{

// One camera per beat maximising the sum of vecvecScore[beat][camera] minus
// dCutPenalty for each change between consecutive beats. Every beat must
// score the same number of cameras.
std::vector<int32_t> VecChooseShots(const std::vector<std::vector<double>> &vecvecScore, double dCutPenalty);

// Beats start at vecBeatStart (ascending frames) and the last ends before
// nfrmEnd. A shot (a run of beats on one camera) shorter than cfrmMin
// frames takes the camera of the shot before it (the first takes the one
// after it), so the picture doesn't flicker between angles.
void HoldShortShots(const std::vector<int32_t> &vecBeatStart, int32_t nfrmEnd, int32_t cfrmMin,
                    std::vector<int32_t> *pvecCamera);

// How well a subject's on-screen height (share of the view height, which
// can pass 1 when the legs are cropped) suits a shot: 1 at dIdeal, falling
// to 0 for tiny figures, more gently for ones that overflow the frame
double DFramingScore(double dHeightShare, double dIdeal = 0.5);

} // namespace automation

#endif // AUTOMATION_DIRECTING_H
