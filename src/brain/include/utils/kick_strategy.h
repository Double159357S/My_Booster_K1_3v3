#pragma once

#include <algorithm>
#include <cmath>

#include "kick_geometry.h"

namespace kick_strategy {

struct AlignmentLimits {
    double minRange = 0.05;
    double maxRange = 1.4;
    double bearing = 0.35;
    double approachError = 0.25;
    double bodyError = 0.25;
};

enum class BallSource { none, real, recentReal, predicted, memory };

struct BallObservation {
    bool detected = false;
    bool known = false;
    bool lost = false;
    double ageMs = 0.0;
    double x = 0.0, y = 0.0;
    bool predictionReliable = false;
    double predictedX = 0.0, predictedY = 0.0;
};

// 启动判断和 /kick_ball 发布共用此选择器。脚边漏检一两帧时沿用最近
// 的真实球坐标；超过短暂宽限期后，不用近距离的旧记忆或预测继续踢。
inline BallSource selectBallSource(const BallObservation &ball, double freshMs,
                                   double holdMs, double nearRejectRange,
                                   bool allowPredicted, bool allowMemory) {
    if (ball.lost || !std::isfinite(ball.ageMs) || ball.ageMs < 0.0 ||
        ball.ageMs > holdMs) return BallSource::none;
    const double range = std::hypot(ball.x, ball.y);
    const bool realValid = std::isfinite(range) && range > 0.05;
    if (realValid && ball.ageMs <= freshMs) {
        if (ball.detected) return BallSource::real;
        if (allowMemory && ball.known) return BallSource::recentReal;
    }
    const double predictedRange = std::hypot(ball.predictedX, ball.predictedY);
    const bool predictionValid = ball.predictionReliable &&
        std::isfinite(predictedRange) && predictedRange > 0.05;
    if ((realValid && range <= nearRejectRange) ||
        (predictionValid && predictedRange <= nearRejectRange)) return BallSource::none;
    if (allowPredicted && predictionValid) return BallSource::predicted;
    if (allowMemory && ball.known && realValid) return BallSource::memory;
    return BallSource::none;
}

inline bool isRecentReal(BallSource source) {
    return source == BallSource::real || source == BallSource::recentReal;
}

// 同时检查机器人到球的路线和身体朝向，球在侧面时不能直接启动踢球。
inline bool alignedForKick(double ballX, double ballY, double robotHeading,
                           double kickDirection, const AlignmentLimits &limits) {
    if (!std::isfinite(ballX) || !std::isfinite(ballY) ||
        !std::isfinite(robotHeading) || !std::isfinite(kickDirection)) return false;
    const double range = std::hypot(ballX, ballY);
    const double bearing = std::atan2(ballY, ballX);
    return ballX > 0.0 && range > limits.minRange && range <= limits.maxRange &&
        std::abs(bearing) <= limits.bearing &&
        kick_geometry::angleDistance(robotHeading + bearing, kickDirection) <= limits.approachError &&
        kick_geometry::angleDistance(robotHeading, kickDirection) <= limits.bodyError;
}

// 减速时的小幅姿态变化不反复取消启动；明显偏转仍会退出重新调整。
inline AlignmentLimits startupAlignmentLimits(AlignmentLimits limits) {
    limits.bearing += 0.05;
    limits.approachError += 0.05;
    limits.bodyError += 0.05;
    limits.maxRange += 0.1;
    return limits;
}

// 启动条件只管进入 SDK。SDK 执行后的调整、转身和短暂漏检由其退出逻辑
// 处理，避免行为树每帧重新套用启动门槛而中断踢球。
inline bool keepVisualKickSession(bool active, bool previousDecisionIsKick,
                                   bool authorized, bool ballOut) {
    return active && previousDecisionIsKick && authorized && !ballOut;
}

inline bool keepDribbleKickSession(bool active, bool previousDecisionIsKick,
                                   bool authorized, bool recentRealBall,
                                   double ballX, bool ballOut) {
    return active && previousDecisionIsKick && authorized && recentRealBall &&
        std::isfinite(ballX) && ballX > 0.0 && !ballOut;
}

// 软开球限速在整个趟球动作中保持，加速按实际时间计算，不依赖 tick 频率。
inline double dribbleSpeedLimit(double requested, bool softKickoff,
                               bool kickingOff, double kickoffLimit) {
    if (!std::isfinite(requested) || requested <= 0.0) return 0.0;
    if (softKickoff && kickingOff) {
        if (!std::isfinite(kickoffLimit) || kickoffLimit <= 0.0) return 0.0;
        return std::min(requested, kickoffLimit);
    }
    return requested;
}

inline double rampDribbleSpeed(double current, double limit, double elapsedMs) {
    if (!std::isfinite(limit) || limit <= 0.0) return 0.0;
    const double dt = std::clamp(elapsedMs, 0.0, 100.0) / 1000.0;
    return std::min(limit, std::max(0.0, current) + 0.8 * dt);
}

inline bool inGoalieClearanceArea(double ballX, double ballY, double fieldLength,
                                  double penaltyLength, double penaltyWidth,
                                  double margin) {
    if (!std::isfinite(ballX) || !std::isfinite(ballY)) return false;
    const double goalLine = -fieldLength / 2.0;
    return ballX >= goalLine - 0.15 &&
        ballX <= std::min(0.0, goalLine + penaltyLength + margin) &&
        std::abs(ballY) <= penaltyWidth / 2.0 + margin;
}

inline bool goalieCanClaim(double ballX, double ballY, double range,
                            double fieldLength, double penaltyLength,
                            double penaltyWidth, double margin, double maxRange) {
    return std::isfinite(range) && range > 0.05 && range <= maxRange &&
        inGoalieClearanceArea(ballX, ballY, fieldLength, penaltyLength, penaltyWidth, margin);
}

enum class ClearanceAction { defend, approach, align, wait, kick };

// 所有主动接近和踢球都要求共享控球权；冷却期间仍可调整，不必退回门线。
inline ClearanceAction clearanceAction(bool authorized, double range,
                                       double visualKickRange, bool aligned,
                                       bool cooldownReady, bool localSourceUsable = true) {
    if (!authorized || !std::isfinite(range) || range <= 0.05) return ClearanceAction::defend;
    if (!localSourceUsable) return ClearanceAction::approach;
    if (range > visualKickRange) return ClearanceAction::approach;
    if (!aligned) return ClearanceAction::align;
    if (!cooldownReady) return ClearanceAction::wait;
    return ClearanceAction::kick;
}

struct AdjustmentVelocity { double x = 0.0, y = 0.0; };

inline double adjustmentHeadingError(double bearing, double approachError,
                                      double bodyError, double turnThreshold) {
    return std::abs(approachError) <= turnThreshold ? bodyError : bearing;
}

inline double effectiveAdjustmentSpeed(double speed, double minimum = 0.3) {
    if (!std::isfinite(speed) || std::abs(speed) <= 1e-5) return 0.0;
    return std::copysign(std::max(std::abs(speed), minimum), speed);
}

// Keep the direction of a two-axis command when making it large enough for
// the walking controller. Raising each component separately distorts the
// orbit direction, especially when one component is close to zero.
inline AdjustmentVelocity effectiveAdjustmentVector(double x, double y,
                                                      double minimum = 0.3) {
    if (!std::isfinite(x) || !std::isfinite(y)) return {};
    const double speed = std::hypot(x, y);
    if (!std::isfinite(speed) || speed <= 1e-5) return {};
    minimum = std::isfinite(minimum) ? std::max(0.0, minimum) : 0.3;
    const double scale = std::max(speed, minimum) / speed;
    return {x * scale, y * scale};
}

// Distance alone cannot determine whether the approach is finished. Keep a
// separate coarse orbit until the route to the ball is close to the shot line.
// Hysteresis avoids switching back and forth near the handoff angle.
inline bool needsReposition(double range, double approachError,
                             bool currentlyRepositioning,
                             double enterAngle = 0.65, double exitAngle = 0.35) {
    if (!std::isfinite(range) || range <= 0.05 ||
        !std::isfinite(approachError)) return false;
    enterAngle = std::isfinite(enterAngle) ?
        std::clamp(enterAngle, 0.1, kick_geometry::kPi) : 0.65;
    exitAngle = std::isfinite(exitAngle) ?
        std::clamp(exitAngle, 0.05, enterAngle - 0.05) :
        std::min(0.35, enterAngle - 0.05);
    return std::abs(kick_geometry::normalizeAngle(approachError)) >
        (currentlyRepositioning ? exitAngle : enterAngle);
}

struct RepositionVelocity {
    double x = 0.0, y = 0.0, yaw = 0.0;
    bool retreat = false;
};

// Coarse positioning keeps the ball ahead of the robot and moves around it,
// rather than commanding a straight path to a point beyond the ball. When
// already too close, first make room for the orbit. The caller can latch the
// orbit direction so noise at +/-pi does not reverse the chosen route.
inline RepositionVelocity repositionVelocity(double range, double bearing,
                                               double approachError,
                                               double orbitDistance,
                                               double maxSpeed, double maxYaw,
                                               int orbitDirection = 0) {
    if (!std::isfinite(range) || range <= 0.05 || !std::isfinite(bearing) ||
        !std::isfinite(approachError) || !std::isfinite(maxSpeed) ||
        maxSpeed <= 0.0 || !std::isfinite(maxYaw) || maxYaw < 0.0) return {};
    orbitDistance = std::isfinite(orbitDistance) ?
        std::clamp(orbitDistance, 0.25, 1.2) : 0.55;
    bearing = kick_geometry::normalizeAngle(bearing);
    approachError = kick_geometry::normalizeAngle(approachError);
    RepositionVelocity result;
    result.yaw = std::abs(bearing) <= 0.05 ? 0.0 :
        std::clamp(effectiveAdjustmentSpeed(1.5 * bearing), -maxYaw, maxYaw);
    // Turn first if the ball is on the side or behind the robot; translating
    // in that pose can obscure the ball and bypass the intended safe orbit.
    if (std::abs(bearing) > 0.5) return result;

    result.retreat = range < orbitDistance - 0.06;
    double radial = 0.0, tangent = 0.0;
    if (result.retreat) {
        radial = -std::min(maxSpeed,
            effectiveAdjustmentSpeed((orbitDistance - range) * 1.5));
    } else {
        radial = std::abs(range - orbitDistance) <= 0.04 ? 0.0 :
            std::clamp((range - orbitDistance) * 1.5, -maxSpeed, maxSpeed);
        if (std::abs(approachError) > 0.02) {
            const int direction = orbitDirection == 0 ?
                (approachError > 0.0 ? 1 : -1) : (orbitDirection > 0 ? 1 : -1);
            tangent = -direction * std::min(maxSpeed,
                std::max(0.3, maxSpeed * 0.75));
        }
    }
    auto translation = effectiveAdjustmentVector(
        radial * std::cos(bearing) - tangent * std::sin(bearing),
        radial * std::sin(bearing) + tangent * std::cos(bearing));
    const double speed = std::hypot(translation.x, translation.y);
    if (speed > maxSpeed) {
        translation.x *= maxSpeed / speed;
        translation.y *= maxSpeed / speed;
    }
    result.x = translation.x;
    result.y = translation.y;
    return result;
}

struct ChaseVelocity { double x = 0.0, yaw = 0.0; };

// 对目标点减速，目标在侧后方时先转身。缩小前向分量后不能再次抬到最低步速。
inline ChaseVelocity chaseVelocity(double targetX, double targetY,
                                   double maxSpeed, double maxYaw,
                                   double stopDistance = 0.08) {
    const double distance = std::hypot(targetX, targetY);
    if (!std::isfinite(distance) || distance <= stopDistance) return {};
    const double bearing = std::atan2(targetY, targetX);
    const double turn = std::abs(bearing) < 0.03 ? 0.0 :
        std::clamp(effectiveAdjustmentSpeed(bearing), -maxYaw, maxYaw);
    const double speed = std::min(maxSpeed, effectiveAdjustmentSpeed(distance - stopDistance));
    return {speed * std::max(0.0, std::cos(bearing)), turn};
}

inline double adjustmentTurnVelocity(double bearing, double approachError,
                                      double bodyError, double turnThreshold,
                                      double factor, double noTurnThreshold) {
    const double error = adjustmentHeadingError(bearing, approachError, bodyError, turnThreshold);
    if (!std::isfinite(error) || std::abs(error) <= std::min(noTurnThreshold, 0.02)) return 0.0;
    return effectiveAdjustmentSpeed(error * factor);
}

// 对准后停止横移；仍需调整时给步态可执行的最低速度，避免小指令被忽略。
// 角度/距离死区抑制噪声，球太近时允许退到调整距离。
inline AdjustmentVelocity adjustmentVelocity(double range, double bearing,
                                              double directionError, double standOff,
                                              double tangentialSpeed, double rampAngle,
                                              double directionTolerance = 0.02) {
    if (!std::isfinite(range) || range <= 0.05 || !std::isfinite(bearing) ||
        !std::isfinite(directionError)) return {};
    const double radial = std::abs(range - standOff) <= 0.04 ? 0.0 :
        std::clamp(range - standOff, -0.3, 0.5);
    const double tangent = std::abs(directionError) <= directionTolerance ? 0.0 :
        -tangentialSpeed * std::clamp(directionError / std::max(0.05, rampAngle), -1.0, 1.0);
    return effectiveAdjustmentVector(radial * std::cos(bearing) - tangent * std::sin(bearing),
                                     radial * std::sin(bearing) + tangent * std::cos(bearing));
}

} // namespace kick_strategy
