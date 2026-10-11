#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace team_cooperation {

inline double steadyNowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Announcement {
    int coordinator = 0;
    int owner = 0;
    std::uint32_t revision = 0;
    std::uint32_t leaseMs = 0;
};

inline bool sameDecision(const Announcement &a, const Announcement &b) {
    return a.coordinator == b.coordinator && a.owner == b.owner &&
        a.revision == b.revision;
}

struct Peer {
    int id = 0;
    bool alive = false;
    bool canControl = false;
    bool kicking = false;
    double cost = std::numeric_limits<double>::infinity();
    double ageMs = std::numeric_limits<double>::infinity();
    Announcement decision;
    bool nearVisibleBall = false; // 本机直接看见近球，优先于只凭共享坐标接近
};

struct Settings {
    double freshMs = 800.0;
    double leaseMs = 1200.0;
    double holdMs = 1500.0;
    double switchMargin = 0.75;
    double confirmMs = 500.0;
    // Covers the existing visual-kick exit ramp (clamped to at most 700 ms).
    double exitGraceMs = 750.0;
};

// One coordinator publishes a decision; peers echo it before the coordinator
// itself acts. Handoffs revoke the old grant before granting the next owner.
class Arbitration {
public:
    const char *phase() const { return phase_; }
    int agreements() const { return agreements_; }
    int quorum() const { return quorum_; }
    int freshCount() const { return freshCount_; }
    Announcement announcement(double nowMs) const {
        auto result = decision_;
        result.leaseMs = result.owner > 0
            ? static_cast<std::uint32_t>(std::max(0.0, validUntil_ - nowMs)) : 0;
        return result;
    }

    bool update(double nowMs, int selfId, int quorum,
                const std::vector<Peer> &peers, const Settings &settings) {
        const auto fresh = [&](const Peer &p) {
            return p.alive && p.ageMs >= 0.0 && p.ageMs <= settings.freshMs;
        };
        int coordinator = 0;
        int freshCount = 0;
        for (const auto &p : peers) {
            if (!fresh(p)) continue;
            ++freshCount;
            if (coordinator == 0 || p.id < coordinator) coordinator = p.id;
        }
        quorum = std::max(1, quorum);
        quorum_ = quorum;
        freshCount_ = freshCount;
        agreements_ = 0;
        phase_ = "no_quorum";
        if (coordinator != decision_.coordinator) {
            const int previousOwner = decision_.owner;
            decision_ = {coordinator, 0, decision_.revision + 1, 0};
            validUntil_ = nowMs;
            ownerSince_ = nowMs;
            challenger_ = 0;
            previousOwner_ = previousOwner;
            revoking_ = coordinator != 0;
            revokedAt_ = nowMs;
            // Wait out grants from the previous authority before a takeover.
            revokeDeadline_ = nowMs + settings.leaseMs + settings.exitGraceMs;
            // A cold coordinator may have missed an existing grant too.
            takeover_ = coordinator != 0;
        }
        if (coordinator == 0 || freshCount < quorum) return false;

        const auto getPeer = [&](int id) -> const Peer * {
            for (const auto &p : peers) if (p.id == id && fresh(p)) return &p;
            return nullptr;
        };
        const auto candidate = [&](const Peer &p) {
            return fresh(p) && p.canControl && std::isfinite(p.cost);
        };

        if (coordinator == selfId) {
            const Peer *best = nullptr;
            for (const auto &p : peers) {
                if (!candidate(p)) continue;
                if (!best || (p.nearVisibleBall && !best->nearVisibleBall) ||
                    (p.nearVisibleBall == best->nearVisibleBall &&
                     (p.cost < best->cost - 0.05 ||
                      (std::fabs(p.cost - best->cost) <= 0.05 && p.id < best->id)))) {
                    best = &p;
                }
            }
            const Peer *owner = getPeer(decision_.owner);
            if (decision_.owner > 0 && (!owner || !candidate(*owner))) {
                revoke(nowMs, settings);
            } else if (decision_.owner > 0 && best && best->id != decision_.owner &&
                       !owner->kicking &&
                       ((best->nearVisibleBall && !owner->nearVisibleBall) ||
                        (best->nearVisibleBall == owner->nearVisibleBall &&
                         nowMs - ownerSince_ >= settings.holdMs &&
                         best->cost + settings.switchMargin < owner->cost))) {
                if (challenger_ != best->id) {
                    challenger_ = best->id;
                    challengeSince_ = nowMs;
                } else if (nowMs - challengeSince_ >= settings.confirmMs) {
                    revoke(nowMs, settings);
                }
            } else {
                challenger_ = 0;
            }

            if (revoking_) {
                const Peer *old = getPeer(previousOwner_);
                const bool acknowledged = old && !old->kicking &&
                    sameDecision(old->decision, decision_);
                const bool stopped = !takeover_ && acknowledged &&
                    nowMs - revokedAt_ >= settings.exitGraceMs;
                if (!stopped && nowMs < revokeDeadline_) {
                    phase_ = "handoff";
                    return false;
                }
                revoking_ = false;
                takeover_ = false;
            }
            if (decision_.owner == 0 && best) {
                decision_.owner = best->id;
                ++decision_.revision;
                ownerSince_ = nowMs;
                challenger_ = 0;
            }
            if (decision_.owner > 0) validUntil_ = nowMs + settings.leaseMs;
        } else {
            const Peer *authority = getPeer(coordinator);
            if (!authority || authority->decision.coordinator != coordinator ||
                authority->decision.leaseMs > settings.leaseMs) {
                phase_ = "no_authority";
                return false;
            }
            decision_ = authority->decision;
            // A stored packet must not renew a grant on every 100 Hz tick.
            validUntil_ = nowMs - authority->ageMs + decision_.leaseMs;
        }

        int agreements = 1; // This robot echoes the adopted proposal.
        for (const auto &p : peers) {
            if (p.id != selfId && fresh(p) && sameDecision(p.decision, decision_)) {
                ++agreements;
            }
        }
        const Peer *self = getPeer(selfId);
        agreements_ = agreements;
        const bool authorized = self && candidate(*self) && decision_.owner == selfId &&
            decision_.owner > 0 && nowMs < validUntil_ && agreements >= quorum;
        phase_ = authorized ? "ready" : decision_.owner == 0 ? "no_candidate" :
            decision_.owner != selfId ? "support" :
            !self || !candidate(*self) ? "ineligible" : "awaiting_echo";
        return authorized;
    }

private:
    void revoke(double nowMs, const Settings &settings) {
        previousOwner_ = decision_.owner;
        decision_.owner = 0;
        ++decision_.revision;
        validUntil_ = nowMs;
        revokedAt_ = nowMs;
        revokeDeadline_ = nowMs + settings.leaseMs + settings.exitGraceMs;
        revoking_ = true;
        takeover_ = false;
        challenger_ = 0;
    }

    Announcement decision_;
    double validUntil_ = 0.0;
    double ownerSince_ = 0.0;
    int challenger_ = 0;
    double challengeSince_ = 0.0;
    int previousOwner_ = 0;
    bool revoking_ = false;
    bool takeover_ = false;
    double revokedAt_ = 0.0;
    double revokeDeadline_ = 0.0;
    int agreements_ = 0, quorum_ = 1, freshCount_ = 0;
    const char *phase_ = "idle";
};

struct Position { double x = 0.0, y = 0.0; };

inline double pointSegmentDistance(Position p, Position a, Position b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double length2 = dx * dx + dy * dy;
    const double t = length2 > 1e-9
        ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length2, 0.0, 1.0) : 0.0;
    return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}

inline double segmentDistance(Position a, Position b, Position c, Position d) {
    const auto cross = [](Position p, Position q, Position r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    if (cross(a, b, c) * cross(a, b, d) < 0.0 &&
        cross(c, d, a) * cross(c, d, b) < 0.0) return 0.0;
    return std::min({pointSegmentDistance(a, c, d), pointSegmentDistance(b, c, d),
                     pointSegmentDistance(c, a, b), pointSegmentDistance(d, a, b)});
}

// Select one side per ownership round, considering the whole route as well as
// the endpoint. The caller clamps both candidates to the actual field first.
inline int supportSide(Position self, Position left, Position right,
                       Position owner, Position ball, double clearance, int preferred) {
    const auto score = [&](Position target) {
        const double ownerClear = pointSegmentDistance(owner, self, target);
        const double ballClear = pointSegmentDistance(ball, self, target);
        const double corridorClear = segmentDistance(self, target, owner, ball);
        return std::min({ownerClear, ballClear, corridorClear, clearance * 2.0});
    };
    const double l = score(left), r = score(right);
    if (std::fabs(l - r) < 0.05) return preferred;
    return l > r ? 1 : -1;
}

// Remove only velocity approaching a teammate; allow retreat and side motion.
// Called after minimum-speed boosts so a zeroed command cannot be boosted again.
inline void limitApproach(double &vx, double &vy, Position teammate,
                          double stopDistance, double slowDistance, double speedLimit) {
    const double range = std::hypot(teammate.x, teammate.y);
    if (!std::isfinite(range) || range >= slowDistance) return;
    if (range < 1e-6) { vx = 0.0; vy = 0.0; return; }
    const double ux = teammate.x / range, uy = teammate.y / range;
    const double approaching = vx * ux + vy * uy;
    const double allowed = speedLimit * std::clamp(
        (range - stopDistance) / std::max(0.01, slowDistance - stopDistance), 0.0, 1.0);
    if (approaching > allowed) {
        vx -= (approaching - allowed) * ux;
        vy -= (approaching - allowed) * uy;
    }
}

inline void limitTeamVelocity(double &vx, double &vy, double &omega,
                              const std::vector<Position> &teammates,
                              double stopDistance, double slowDistance,
                              double speedLimit, double maxX, double maxY) {
    vx = std::clamp(vx, -maxX, maxX);
    vy = std::clamp(vy, -maxY, maxY);
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto &p : teammates) {
            limitApproach(vx, vy, p, stopDistance, slowDistance, speedLimit);
            if (std::hypot(p.x, p.y) < stopDistance) omega = 0.0;
        }
    }
    // Component clipping could turn a safe tangent back toward a teammate.
    // Uniform scaling preserves all approach constraints and speed limits.
    double scale = 1.0;
    if (std::fabs(vx) > maxX) scale = std::min(scale, maxX / std::fabs(vx));
    if (std::fabs(vy) > maxY) scale = std::min(scale, maxY / std::fabs(vy));
    vx *= scale;
    vy *= scale;
    for (const auto &p : teammates) {
        double checkedX = vx, checkedY = vy;
        limitApproach(checkedX, checkedY, p, stopDistance, slowDistance, speedLimit);
        if (std::hypot(checkedX - vx, checkedY - vy) > 1e-5) {
            vx = 0.0;
            vy = 0.0;
            break;
        }
    }
}

// 视觉踢球会移动脚和身体。很近的队友始终阻止启动；其余只检查
// 到球的路线和向前清球的通道，后方队友不再触发整圈停球。
inline bool blocksKick(Position teammate, Position ball, double direction,
                       double stopDistance) {
    const double range = std::hypot(teammate.x, teammate.y);
    if (range >= stopDistance) return false;
    if (range < stopDistance * 0.6) return true;
    const double corridor = stopDistance * 0.6;
    const Position forward{std::cos(direction), std::sin(direction)};
    return pointSegmentDistance(teammate, {}, ball) < corridor ||
        pointSegmentDistance(teammate, {}, forward) < corridor;
}

inline bool mustYield(Position self, Position owner, Position ball,
                      double clearance) {
    return std::hypot(self.x - owner.x, self.y - owner.y) < clearance ||
        (pointSegmentDistance(self, owner, ball) < clearance &&
         std::hypot(self.x - ball.x, self.y - ball.y) < 1.6);
}

// 从可行方向中选择离主控和到球路线更远的一步。先应用相同的
// 多队友避碰约束，避免普通 Assist 的目标或避障方向把让行者卡住。
// 返回场地坐标速度；没有安全出口时保持停止，不强行穿过队友。
inline Position yieldVelocity(Position self, Position owner, Position ball,
                              const std::vector<Position> &others,
                              double fieldLength, double fieldWidth,
                              double stopDistance, double slowDistance,
                              double approachLimit, int preferredSide) {
    Position best{};
    double bestScore = 0.0;
    const double before = std::hypot(self.x - owner.x, self.y - owner.y);
    const double corridorBefore = pointSegmentDistance(self, owner, ball);
    auto outside = [&](Position p) {
        return std::max(0.0, std::fabs(p.x) - (fieldLength / 2.0 - 0.2)) +
            std::max(0.0, std::fabs(p.y) - (fieldWidth / 2.0 - 0.2));
    };
    for (int i = 0; i < 32; ++i) {
        const double angle = i * 2.0 * std::acos(-1.0) / 32.0;
        double vx = 0.4 * std::cos(angle), vy = 0.4 * std::sin(angle), omega = 0.0;
        limitTeamVelocity(vx, vy, omega, others, stopDistance, slowDistance,
            approachLimit, 0.4, 0.4);
        if (std::hypot(vx, vy) < 0.25) continue;
        const Position next{self.x + vx, self.y + vy};
        if (outside(next) > outside(self) + 1e-6) continue;
        const double gain = std::hypot(next.x - owner.x, next.y - owner.y) - before;
        if (gain < 0.01) continue;
        const double score = gain + 0.5 *
            (pointSegmentDistance(next, owner, ball) - corridorBefore) +
            0.03 * preferredSide * vy;
        if (score > bestScore) { best = {vx, vy}; bestScore = score; }
    }
    return best;
}

} // namespace team_cooperation
