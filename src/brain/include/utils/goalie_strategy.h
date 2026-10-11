#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "kick_strategy.h"

namespace goalie_strategy {

// 清球仍属于守门任务。SDK 启动和退出期间不能因 kicking=true 被改成前锋。
inline bool protectClearanceRole(bool goalie, bool ballKnown, bool kicking,
                                  double ballX, double ballY, double range,
                                  double fieldLength, double penaltyLength,
                                  double penaltyWidth, double margin, double maxRange) {
    return goalie && (ballKnown || kicking) && std::isfinite(range) &&
        range > 0.05 && range <= maxRange + (kicking ? 0.5 : 0.0) &&
        kick_strategy::inGoalieClearanceArea(ballX, ballY, fieldLength,
                                             penaltyLength, penaltyWidth, margin);
}

struct Candidate {
    int id = 0;
    double distanceToGoal = 0.0;
    bool kicking = false;
    bool clearing = false;
};

// 调用方只传入未受罚、位姿新鲜的球员。禁区清球者优先；否则沿用
// 距球门最近的非踢球球员，全部正在踢球时才从所有候选人中选择。
inline int select(const std::vector<Candidate> &candidates, double tieMargin) {
    const auto priority = [](const Candidate &candidate) {
        return candidate.clearing ? 0 : candidate.kicking ? 2 : 1;
    };
    const auto valid = [](const Candidate &candidate) {
        return candidate.id > 0 && std::isfinite(candidate.distanceToGoal) &&
            candidate.distanceToGoal >= 0.0;
    };
    int bestPriority = 3;
    double minimumDistance = std::numeric_limits<double>::infinity();
    for (const auto &candidate : candidates) {
        if (!valid(candidate)) continue;
        const int candidatePriority = priority(candidate);
        if (candidatePriority < bestPriority) {
            bestPriority = candidatePriority;
            minimumDistance = candidate.distanceToGoal;
        } else if (candidatePriority == bestPriority) {
            minimumDistance = std::min(minimumDistance, candidate.distanceToGoal);
        }
    }
    // 两遍选择使用同一个最近距离。逐个“差不多就按号比”不满足传递性，
    // self-first 的不同遍历顺序会让三台选出不同的守门员。
    const double margin = std::isfinite(tieMargin) ? std::max(0.0, tieMargin) : 0.0;
    int bestId = 0;
    for (const auto &candidate : candidates) {
        if (valid(candidate) && priority(candidate) == bestPriority &&
            candidate.distanceToGoal <= minimumDistance + margin &&
            (bestId == 0 || candidate.id < bestId)) bestId = candidate.id;
    }
    return bestId;
}

} // namespace goalie_strategy
