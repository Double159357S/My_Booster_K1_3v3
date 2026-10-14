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

struct RecoverySettings {
    double stopDistance = 0.65;
    double turnAngle = 0.6;
    double yawLimit = 0.8;
    double scanDelayMs = 400.0;
    double scanStepMs = 250.0;
    double scanYaw = 0.35;
    double scanPitch = 0.15;
    double timeoutMs = 2500.0;
    double targetMoveDistance = 0.4;

    RecoverySettings sanitized() const {
        const auto bounded = [](double value, double fallback, double low, double high) {
            return std::clamp(std::isfinite(value) ? value : fallback, low, high);
        };
        RecoverySettings result;
        result.stopDistance = bounded(stopDistance, 0.65, 0.3, 1.2);
        result.turnAngle = bounded(turnAngle, 0.6, 0.25, 1.2);
        result.yawLimit = bounded(yawLimit, 0.8, 0.1, 1.5);
        result.scanDelayMs = bounded(scanDelayMs, 400.0, 0.0, 2000.0);
        result.scanStepMs = bounded(scanStepMs, 250.0, 100.0, 1000.0);
        result.scanYaw = bounded(scanYaw, 0.35, 0.0, 0.8);
        result.scanPitch = bounded(scanPitch, 0.15, 0.0, 0.3);
        result.targetMoveDistance = bounded(targetMoveDistance, 0.4, 0.15, 1.5);
        // 超时至少覆盖定向注视，以及完整的六步扫描。
        result.timeoutMs = std::max(
            bounded(timeoutMs, 2500.0, 500.0, 15000.0),
            result.scanDelayMs + 6.0 * result.scanStepMs);
        return result;
    }
};

enum class RecoveryPhase { inactive, support, turn, approach, scan, released };

struct HeadOffset { double yaw = 0.0, pitch = 0.0; };

inline double approachSpeed(double range, double limit, double stopDistance = 0.65) {
    if (!std::isfinite(range) || !std::isfinite(limit) || limit <= 0.0) return 0.0;
    stopDistance = std::clamp(std::isfinite(stopDistance) ? stopDistance : 0.65, 0.3, 1.2);
    if (range <= stopDistance) return 0.0;
    // 接近到共享坐标附近便停步；只有本机视觉才能确认脚边的球。
    return std::min(limit, std::max(0.3, range - stopDistance + 0.05));
}

// 主控接近共享球后仍看不见，释放资格给看见球的队友。
// 同一个目标的消息刷新、来源切换与小幅抖动不重新开始扫描预算。
class Recovery {
public:
    bool canPursue(double nowMs, bool sharedOnly, bool ownerAttempt, double range,
                    double x, double y, double bearing = 0.0, bool reliable = true,
                    const RecoverySettings &settings = {}) {
        if (!sharedOnly) { reset(); return true; }
        if (!reliable || !validTime(nowMs) || !std::isfinite(x) || !std::isfinite(y) ||
            !validGeometry(range, bearing)) {
            if (validTime(nowMs)) accrue(nowMs);
            suspend();
            return false;
        }
        const auto clean = settings.sanitized();
        if (!targetValid_ || std::hypot(x - x_, y - y_) > clean.targetMoveDistance) {
            reset();
            targetValid_ = true;
            x_ = x;
            y_ = y;
        }
        observedReliable_ = true;
        updatePhase(nowMs, ownerAttempt, range, bearing, clean);
        if (!blocked_ && ownerAttempt && nearTarget_ &&
            (scanElapsedMs_ >= clean.timeoutMs || nearElapsedMs_ >= 2.0 * clean.timeoutMs)) {
            // 通常在完整扫描后释放；无法对准时也有有限的近球尝试预算。
            blocked_ = true;
            setPhase(RecoveryPhase::released, nowMs);
        }
        return !blocked_;
    }

    // 仲裁后仅同步实际授权与动作阶段；资格超时仍在下一帧仲裁之前判断。
    void syncOwner(double nowMs, bool ownerAttempt, double range, double bearing,
                    const RecoverySettings &settings = {}) {
        if (!observedReliable_ || !targetValid_ || !validTime(nowMs) ||
            !validGeometry(range, bearing)) {
            if (validTime(nowMs)) accrue(nowMs);
            suspend();
            return;
        }
        updatePhase(nowMs, ownerAttempt, range, bearing, settings.sanitized());
    }

    RecoveryPhase phase() const { return phase_; }

    const char *phaseName() const {
        switch (phase_) {
            case RecoveryPhase::support: return "support";
            case RecoveryPhase::turn: return "turn";
            case RecoveryPhase::approach: return "approach";
            case RecoveryPhase::scan: return "scan";
            case RecoveryPhase::released: return "released";
            default: return "inactive";
        }
    }

    double scanElapsedMs(double nowMs) const {
        if (!std::isfinite(nowMs) || nowMs < 0.0) return 0.0;
        return scanElapsedMs_ + (phase_ == RecoveryPhase::scan && observedReliable_
            ? elapsedSince(nowMs, lastUpdateMs_) : 0.0);
    }

    double phaseElapsedMs(double nowMs) const {
        if (phase_ == RecoveryPhase::inactive) return 0.0;
        return phase_ == RecoveryPhase::scan ? scanElapsedMs(nowMs)
            : elapsedSince(nowMs, phaseSinceMs_);
    }

    HeadOffset headOffset(double nowMs, const RecoverySettings &settings = {}) const {
        if (!observedReliable_ || (phase_ != RecoveryPhase::support && phase_ != RecoveryPhase::scan) ||
            !std::isfinite(nowMs) || nowMs < 0.0) return {};
        const auto clean = settings.sanitized();
        const double elapsed = phaseElapsedMs(nowMs) - clean.scanDelayMs;
        if (elapsed < 0.0 || elapsed >= 6.0 * clean.scanStepMs) return {};
        const int step = static_cast<int>(elapsed / clean.scanStepMs);
        switch (step) {
            case 1: return {clean.scanYaw, 0.0};
            case 2: return {-clean.scanYaw, 0.0};
            case 3: return {0.0, -clean.scanPitch};
            case 4: return {0.0, clean.scanPitch};
            default: return {};
        }
    }

private:
    static bool validGeometry(double range, double bearing) {
        return std::isfinite(range) && range >= 0.0 && std::isfinite(bearing);
    }

    bool validTime(double nowMs) const {
        return std::isfinite(nowMs) && nowMs >= 0.0 &&
            (lastUpdateMs_ < 0.0 || nowMs >= lastUpdateMs_);
    }

    static double elapsedSince(double nowMs, double sinceMs) {
        return std::isfinite(nowMs) && sinceMs >= 0.0 && nowMs >= sinceMs
            ? nowMs - sinceMs : 0.0;
    }

    void setPhase(RecoveryPhase next, double nowMs) {
        if (phase_ != next) {
            phase_ = next;
            phaseSinceMs_ = nowMs;
        }
    }

    void updatePhase(double nowMs, bool ownerAttempt, double range, double bearing,
                        const RecoverySettings &settings) {
        accrue(nowMs);
        if (blocked_) { setPhase(RecoveryPhase::released, nowMs); return; }
        if (!ownerAttempt) {
            nearTarget_ = false;
            setPhase(RecoveryPhase::support, nowMs);
            return;
        }
        nearTarget_ = nearTarget_ ? range <= settings.stopDistance + 0.12
            : range <= settings.stopDistance;
        const double yaw = std::abs(std::atan2(std::sin(bearing), std::cos(bearing)));
        if (nearTarget_) {
            const double alignedAngle = phase_ == RecoveryPhase::scan ? 0.35 : 0.2;
            setPhase(yaw <= alignedAngle ? RecoveryPhase::scan : RecoveryPhase::turn, nowMs);
        } else {
            const double turnAngle = phase_ == RecoveryPhase::turn
                ? settings.turnAngle - 0.15 : settings.turnAngle;
            setPhase(yaw > turnAngle ? RecoveryPhase::turn : RecoveryPhase::approach, nowMs);
        }
    }

    void accrue(double nowMs) {
        const double delta = elapsedSince(nowMs, lastUpdateMs_);
        // 累计实际扫描时间，转向/失去授权/通信中断时暂停，不抹掉已用预算。
        if (observedReliable_ && phase_ == RecoveryPhase::scan) scanElapsedMs_ += delta;
        if (observedReliable_ && nearTarget_ &&
            (phase_ == RecoveryPhase::turn || phase_ == RecoveryPhase::scan)) nearElapsedMs_ += delta;
        lastUpdateMs_ = nowMs;
    }

    void suspend() {
        observedReliable_ = false;
        nearTarget_ = false;
        phase_ = RecoveryPhase::inactive;
        phaseSinceMs_ = lastUpdateMs_ = -1.0;
    }

    void reset() {
        suspend();
        targetValid_ = blocked_ = false;
        scanElapsedMs_ = nearElapsedMs_ = 0.0;
        x_ = y_ = 0.0;
    }

    bool targetValid_ = false, blocked_ = false, observedReliable_ = false, nearTarget_ = false;
    RecoveryPhase phase_ = RecoveryPhase::inactive;
    double phaseSinceMs_ = -1.0, lastUpdateMs_ = -1.0;
    double scanElapsedMs_ = 0.0, nearElapsedMs_ = 0.0;
    double x_ = 0.0, y_ = 0.0;
};

} // namespace team_ball
