#pragma once

#include <algorithm>
#include "geometry_types.h"

namespace ready_formation {

// READY 站位只用同一份裁判机名单，不能根据到球成本、在线队友数或
// 动态守门员重新排名。一轮 READY 内锁定名单，队友暂时丢包不换位置。
class Assignment {
public:
    void update(bool ready, int gcGoalkeeper, int playerCount) {
        if (!ready) {
            goalkeeper_ = 0;
            players_ = 0;
        } else if (goalkeeper_ == 0) {
            players_ = std::max(1, playerCount);
            // 常规 3v3：1 号守门、2/3 号前锋；有效裁判机编号优先。
            goalkeeper_ = gcGoalkeeper >= 1 && gcGoalkeeper <= players_
                ? gcGoalkeeper : 1;
        }
    }

    bool active() const { return goalkeeper_ > 0; }
    int goalkeeper() const { return goalkeeper_; }
    int strikerRank(int playerId) const {
        if (!active() || playerId < 1 || playerId > players_ || playerId == goalkeeper_)
            return -1;
        return playerId - 1 - (playerId > goalkeeper_ ? 1 : 0);
    }

private:
    int goalkeeper_ = 0;
    int players_ = 0;
};

template <typename Field>
Pose2D strikerTarget(int rank, bool ourKickoff, const Field &field) {
    // 小编号前锋靠近己方中圈；另一台在右侧后方，两个目标相距约 2.06m。
    // 对方开球时整体后撤，避免站入中圈。坐标始终使用进攻方向 +x。
    const double frontX = -field.circleRadius - (ourKickoff ? 0.4 : 0.85);
    if (rank <= 0) return {frontX, 0.0, 0.0};
    const double sideY = std::min(1.8, field.width / 2.0 - 0.6);
    const double x = std::max(-field.length / 2.0 + 0.6,
        frontX - 1.0 - ((rank - 1) / 2) * 1.2);
    return {x, rank % 2 == 1 ? -sideY : sideY, 0.0};
}

} // namespace ready_formation
