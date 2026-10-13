#pragma once

#include <cmath>
#include <string>

namespace kick_strategy {

enum class StartState { idle, pending, accepted, rejected, timedOut };

// UUID matching prevents a delayed response from reviving a cancelled/replaced
// request. The caller serializes access; all times use the monotonic clock.
class VisualKickRequest {
public:
    void begin(const std::string &uuid, double nowMs) {
        uuid_ = uuid;
        sentMs_ = nowMs;
        state_ = uuid.empty() || !std::isfinite(nowMs) ? StartState::rejected : StartState::pending;
    }
    void cancel() { uuid_.clear(); state_ = StartState::idle; }
    bool complete(const std::string &uuid, int status, double nowMs, double timeoutMs) {
        if (uuid != uuid_ || state(nowMs, timeoutMs) != StartState::pending) return false;
        state_ = status == 0 ? StartState::accepted : StartState::rejected;
        return true;
    }
    StartState state(double nowMs, double timeoutMs) {
        if (state_ == StartState::pending &&
            (!std::isfinite(nowMs) || !std::isfinite(timeoutMs) || timeoutMs <= 0.0 ||
             nowMs < sentMs_ || nowMs - sentMs_ >= timeoutMs)) {
            state_ = StartState::timedOut;
        }
        return state_;
    }
private:
    std::string uuid_;
    double sentMs_ = 0.0;
    StartState state_ = StartState::idle;
};

} // namespace kick_strategy
