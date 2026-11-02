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

std::vector<int32_t> VecChooseShotsPaced(const std::vector<std::vector<double>> &vecvecScore,
                                         const std::vector<int32_t> &vecBeatStart, int32_t nfrmEnd,
                                         const std::vector<bool> &vecSpeakerChange, const ShotPacing &pacing)
{
    size_t cbeat = vecvecScore.size();
    if (cbeat == 0 || vecvecScore[0].empty() || vecBeatStart.size() < cbeat)
        return {};
    size_t ccam = vecvecScore[0].size();
    auto beatEnd = [&](size_t ibeat) { return ibeat + 1 < cbeat ? vecBeatStart[ibeat + 1] : nfrmEnd; };

    // Viterbi as in VecChooseShots, also remembering where each candidate
    // shot started so a held shot can pay for its length (exact for the
    // best path into each state, which is what matters here)
    std::vector<std::vector<double>> vecvecUsable = vecvecScore;
    for (std::vector<double> &vecScores : vecvecUsable)
    {
        double dBestHere = *std::max_element(vecScores.begin(), vecScores.end());
        for (double &d : vecScores)
        {
            if (d < dBestHere - pacing.dComparable)
                d = -1e9;
        }
    }
    std::vector<std::vector<double>> vecvecBest(cbeat, std::vector<double>(ccam));
    std::vector<std::vector<int32_t>> vecvecFrom(cbeat, std::vector<int32_t>(ccam, -1));
    std::vector<std::vector<int32_t>> vecvecShotStart(cbeat, std::vector<int32_t>(ccam, vecBeatStart[0]));
    auto holdCost = [&](int32_t nfrmShotStart, size_t ibeat) {
        int32_t nfrmLimit = nfrmShotStart + pacing.cfrmHoldMax;
        int32_t cfrmOver = beatEnd(ibeat) - std::max(nfrmLimit, vecBeatStart[ibeat]);
        return cfrmOver > 0 ? cfrmOver * pacing.dHoldPenaltyPerFrame : 0.0;
    };
    for (size_t icam = 0; icam < ccam; icam++)
        vecvecBest[0][icam] = vecvecUsable[0][icam] - holdCost(vecBeatStart[0], 0);
    for (size_t ibeat = 1; ibeat < cbeat; ibeat++)
    {
        bool fSpeakerChange = ibeat < vecSpeakerChange.size() && vecSpeakerChange[ibeat];
        double dCut = pacing.dCutPenalty * (fSpeakerChange ? pacing.dSpeakerCutShare : 1.0);
        for (size_t icam = 0; icam < ccam; icam++)
        {
            double dBest = -1e18;
            int32_t icamFrom = 0, nfrmShotStart = vecBeatStart[ibeat];
            for (size_t icamPrev = 0; icamPrev < ccam; icamPrev++)
            {
                bool fSame = icamPrev == icam;
                int32_t nfrmStart = fSame ? vecvecShotStart[ibeat - 1][icamPrev] : vecBeatStart[ibeat];
                double d = vecvecBest[ibeat - 1][icamPrev] - (fSame ? 0 : dCut) - holdCost(nfrmStart, ibeat);
                if (d > dBest)
                {
                    dBest = d;
                    icamFrom = static_cast<int32_t>(icamPrev);
                    nfrmShotStart = nfrmStart;
                }
            }
            vecvecBest[ibeat][icam] = dBest + vecvecUsable[ibeat][icam];
            vecvecFrom[ibeat][icam] = icamFrom;
            vecvecShotStart[ibeat][icam] = nfrmShotStart;
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
                    std::vector<int32_t> *pvecCamera, const std::vector<std::vector<double>> *pvecvecScore)
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
        if (pvecvecScore != nullptr && ibeatShort > 0 && ibeatShortLim < cbeat)
        {
            // Both neighbours could take it: the one that shows it better
            auto total = [&](int32_t icamT) {
                double d = 0;
                for (size_t ibeat = ibeatShort; ibeat < ibeatShortLim; ibeat++)
                    d += (*pvecvecScore)[ibeat][icamT];
                return d;
            };
            if (total(vecCamera[ibeatShortLim]) > total(icam))
                icam = vecCamera[ibeatShortLim];
        }
        for (size_t ibeat = ibeatShort; ibeat < ibeatShortLim; ibeat++)
            vecCamera[ibeat] = icam;
    }
}

// Past the ideal size the score falls this fast (per ideal-height of excess):
// a face pressed against the lens reads worse than a figure a little small
const double kdOversizeFalloff = 1.5;

double DFramingScore(double dHeightShare, double dIdeal)
{
    if (dHeightShare <= 0 || dIdeal <= 0)
        return 0;
    double d = dHeightShare < dIdeal ? dHeightShare / dIdeal : 1 - (dHeightShare - dIdeal) / dIdeal * kdOversizeFalloff;
    return std::clamp(d, 0.0, 1.0);
}

} // namespace automation
