#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace team_ball {

// 时间为接收机本地的消息年龄，不比较不同机器人的时钟。
struct Report {
    int playerId = 0;
    bool alive = false, detected = false, known = false;
    double ageMs = std::numeric_limits<double>::infinity();
    double confidence = 0.0, range = 0.0, x = 0.0, y = 0.0;
};

inline bool freshLocal(bool known, bool fromTeammate, double ageMs, double freshMs) {
    return known && !fromTeammate && std::isfinite(ageMs) &&
        ageMs >= 0.0 && ageMs <= freshMs;
}

// 只使用新鲜的直接视觉报告。近球也能共享；队友转发的共享球不能
// 再冒充直接检测，否则三台会在无人看见球后一直互相刷新旧坐标。
inline int selectReport(const std::vector<Report> &reports, double freshMs,
                         double confidenceThreshold, double fieldLength, double fieldWidth) {
    int selected = -1;
    for (int i = 0; i < static_cast<int>(reports.size()); ++i) {
        const auto &r = reports[i];
        if (!r.alive || !r.detected || !r.known || !std::isfinite(r.ageMs) ||
            r.ageMs < 0.0 || r.ageMs > freshMs || !std::isfinite(r.confidence) ||
            r.confidence < confidenceThreshold || !std::isfinite(r.range) ||
            r.range <= 0.05 || !std::isfinite(r.x) || !std::isfinite(r.y) ||
            std::abs(r.x) > fieldLength / 2.0 + 0.7 ||
            std::abs(r.y) > fieldWidth / 2.0 + 0.7) continue;
        if (selected < 0 || r.range < reports[selected].range - 0.05 ||
            (std::abs(r.range - reports[selected].range) <= 0.05 &&
             r.playerId < reports[selected].playerId)) selected = i;
    }
    return selected;
}

inline double approachSpeed(double range, double limit) {
    if (!std::isfinite(range) || range <= 0.65) return 0.0;
    // 场地定位有误差，只按共享坐标接近到 0.6 m，随后用本机视觉接球。
    return std::min(std::max(0.0, limit), std::max(0.3, range - 0.6));
}

// 主控接近共享球后仍看不见，释放资格给看见球的队友。
// 不反复抢回同一个静止目标；球明显移动或本机重新看见球后恢复资格。
class Recovery {
public:
    bool canPursue(double nowMs, bool sharedOnly, bool owner, double range,
                    double x, double y) {
        if (!sharedOnly) { reset(); return true; }
        if (!targetValid_ || std::hypot(x - x_, y - y_) > 0.4) {
            reset(); targetValid_ = true; x_ = x; y_ = y;
        }
        if (blocked_) return false;
        if (!owner || range > 0.65) { waitingSince_ = -1.0; return true; }
        if (waitingSince_ < 0.0) waitingSince_ = nowMs;
        if (nowMs - waitingSince_ >= 2000.0) blocked_ = true;
        return !blocked_;
    }
private:
    void reset() { targetValid_ = blocked_ = false; waitingSince_ = -1.0; }
    bool targetValid_ = false, blocked_ = false;
    double waitingSince_ = -1.0, x_ = 0.0, y_ = 0.0;
};

} // namespace team_ball
