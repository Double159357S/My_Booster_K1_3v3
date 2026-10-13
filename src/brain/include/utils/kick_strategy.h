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
    return {effectiveAdjustmentSpeed(radial * std::cos(bearing) - tangent * std::sin(bearing)),
            effectiveAdjustmentSpeed(radial * std::sin(bearing) + tangent * std::cos(bearing))};
}

} // namespace kick_strategy
