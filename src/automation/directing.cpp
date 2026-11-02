/***************************************************************************
    Automation bridge: shot choice (see directing.h).
***************************************************************************/
#include "directing.h"

#include <algorithm>
#include <cmath>

namespace automation
{

std::vector<int32_t> VecChooseShots(const std::vector<std::vector<double>> &vecvecScore, double dCutPenalty)
{
    size_t cbeat = vecvecScore.size();
    if (cbeat == 0 || vecvecScore[0].empty())
        return {};
    size_t ccam = vecvecScore[0].size();

    // Viterbi: best total ending on each camera, and where it came from
    std::vector<std::vector<double>> vecvecBest(cbeat, std::vector<double>(ccam));
    std::vector<std::vector<int32_t>> vecvecFrom(cbeat, std::vector<int32_t>(ccam, -1));
    vecvecBest[0] = vecvecScore[0];
    for (size_t ibeat = 1; ibeat < cbeat; ibeat++)
    {
        for (size_t icam = 0; icam < ccam; icam++)
        {
            double dBest = -1e18;
            int32_t icamFrom = 0;
            for (size_t icamPrev = 0; icamPrev < ccam; icamPrev++)
            {
                double d = vecvecBest[ibeat - 1][icamPrev] - (icamPrev == icam ? 0 : dCutPenalty);
                if (d > dBest)
                {
                    dBest = d;
                    icamFrom = static_cast<int32_t>(icamPrev);
                }
            }
            vecvecBest[ibeat][icam] = dBest + vecvecScore[ibeat][icam];
            vecvecFrom[ibeat][icam] = icamFrom;
        }
    }

    std::vector<int32_t> vecCamera(cbeat);
    vecCamera[cbeat - 1] = static_cast<int32_t>(
        std::max_element(vecvecBest[cbeat - 1].begin(), vecvecBest[cbeat - 1].end()) - vecvecBest[cbeat - 1].begin());
    for (size_t ibeat = cbeat - 1; ibeat > 0; ibeat--)
        vecCamera[ibeat - 1] = vecvecFrom[ibeat][vecCamera[ibeat]];
    return vecCamera;
}

void HoldShortShots(const std::vector<int32_t> &vecBeatStart, int32_t nfrmEnd, int32_t cfrmMin,
                    std::vector<int32_t> *pvecCamera)
{
    std::vector<int32_t> &vecCamera = *pvecCamera;
    size_t cbeat = std::min(vecBeatStart.size(), vecCamera.size());
    // Merge the shortest offending shot first, then look again: merging
    // changes the lengths of its neighbours
    for (;;)
    {
        size_t ibeatShort = cbeat, ibeatShortLim = cbeat;
        int32_t cfrmShortest = cfrmMin;
        size_t cshot = 0;
        for (size_t ibeat = 0; ibeat < cbeat;)
        {
            size_t ibeatLim = ibeat + 1;
            while (ibeatLim < cbeat && vecCamera[ibeatLim] == vecCamera[ibeat])
                ibeatLim++;
            int32_t nfrmLim = ibeatLim < cbeat ? vecBeatStart[ibeatLim] : nfrmEnd;
            int32_t cfrm = nfrmLim - vecBeatStart[ibeat];
            if (cfrm < cfrmShortest)
            {
                cfrmShortest = cfrm;
                ibeatShort = ibeat;
                ibeatShortLim = ibeatLim;
            }
            cshot++;
            ibeat = ibeatLim;
        }
        if (ibeatShort == cbeat || cshot < 2)
            return;
        int32_t icam = ibeatShort > 0 ? vecCamera[ibeatShort - 1] : vecCamera[ibeatShortLim];
        for (size_t ibeat = ibeatShort; ibeat < ibeatShortLim; ibeat++)
            vecCamera[ibeat] = icam;
    }
}

double DFramingScore(double dHeightShare, double dIdeal)
{
    if (dHeightShare <= 0 || dIdeal <= 0)
        return 0;
    double d = dHeightShare < dIdeal ? dHeightShare / dIdeal : 1 - (dHeightShare - dIdeal) / dIdeal * 0.6;
    return std::clamp(d, 0.0, 1.0);
}

} // namespace automation
