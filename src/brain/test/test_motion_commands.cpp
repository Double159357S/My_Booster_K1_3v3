#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "utils/kick_strategy.h"
#include "utils/observation_geometry.h"

namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

struct ApproachResult {
    bool ready = false;
    int steps = 0, retreats = 0, orbits = 0, adjustments = 0;
    double minRange = std::numeric_limits<double>::infinity();
    double handoffError = 0.0;
};

Pose2D approachPose(double range, double routeError,
                    double bearing, double kickDirection = 0.0,
                    Point ball = {}) {
    const double ballDirection = kickDirection - routeError;
    return {ball.x - range * std::cos(ballDirection),
            ball.y - range * std::sin(ballDirection),
            kick_geometry::normalizeAngle(ballDirection - bearing)};
}

// Integrate commands in field coordinates so a plausible-looking local
// velocity cannot hide walking through the ball or circling indefinitely.
// A 0.2 per-axis actuator dead zone reproduces the walking-controller stall.
ApproachResult runApproach(Pose2D robot, Point fieldBall = {},
                           double kickDirection = 0.0, double deadZone = 0.0) {
    using namespace kick_strategy;
    using namespace observation_geometry;
    enum class Stage { chase, reposition, adjust };
    Stage previous = Stage::adjust;
    int orbitDirection = 0;
    ApproachResult result;
    const auto actuator = [deadZone](double value) {
        return std::abs(value) < deadZone ? 0.0 : value;
    };
    for (int step = 0; step < 4000; ++step) {
        result.steps = step;
        const auto ball = toLocal(fieldBall, robot);
        const double range = std::hypot(ball.x, ball.y);
        const double bearing = std::atan2(ball.y, ball.x);
        const double error = kick_geometry::normalizeAngle(kickDirection -
            std::atan2(fieldBall.y - robot.y, fieldBall.x - robot.x));
        const double bodyError = kick_geometry::normalizeAngle(kickDirection - robot.theta);
        result.minRange = std::min(result.minRange, range);
        if (range <= 0.5 && alignedForKick(ball.x, ball.y, robot.theta,
                                         kickDirection, AlignmentLimits{})) {
            result.ready = true;
            break;
        }
        double vx, vy = 0.0, yaw;
        Stage stage;
        if (range > (previous == Stage::chase ? 0.9 : 1.0)) {
            stage = Stage::chase;
            // The production Chase subtree now aims at the final 0.4 m
            // stand-off instead of approaching only 0.1 m behind the ball.
            const auto target = toLocal({fieldBall.x - 0.4 * std::cos(kickDirection),
                                        fieldBall.y - 0.4 * std::sin(kickDirection), 0.0}, robot);
            const auto command = chaseVelocity(target.x, target.y, 1.6, 1.5);
            vx = command.x;
            yaw = command.yaw;
        } else if (needsReposition(range, error, previous == Stage::reposition)) {
            stage = Stage::reposition;
            if (previous != stage) orbitDirection = error >= 0.0 ? 1 : -1;
            const auto command = repositionVelocity(range, bearing, error,
                                                     0.55, 0.45, 1.5, orbitDirection);
            check(std::hypot(command.x, command.y) <= 0.45 + 1e-9,
                  "coarse approach exceeds translation speed limit");
            vx = command.x;
            vy = command.y;
            yaw = command.yaw;
            if (command.retreat) ++result.retreats;
            else if (std::hypot(vx, vy) > 1e-5) ++result.orbits;
        } else {
            stage = Stage::adjust;
            if (previous == Stage::reposition) {
                result.handoffError = std::abs(error);
                check(result.handoffError <= 0.35,
                      "coarse positioning hands a large route error to final adjustment");
            }
            const auto command = adjustmentVelocity(range, bearing, error,
                                                     0.4, 0.15, 0.5, 0.25);
            vx = command.x;
            vy = command.y;
            yaw = std::clamp(adjustmentTurnVelocity(bearing, error, bodyError,
                                                   0.25, 1.5, 0.1), -1.5, 1.5);
            ++result.adjustments;
        }
        vx = actuator(vx);
        vy = actuator(vy);
        yaw = actuator(yaw);
        robot.x += (vx * std::cos(robot.theta) - vy * std::sin(robot.theta)) * 0.01;
        robot.y += (vx * std::sin(robot.theta) + vy * std::cos(robot.theta)) * 0.01;
        robot.theta = kick_geometry::normalizeAngle(robot.theta + yaw * 0.01);
        previous = stage;
    }
    return result;
}
}

int main() {
    try {
        using namespace kick_strategy;
        const auto front = chaseVelocity(2.0, 0.0, 1.6, 1.5);
        check(near(front.x, 1.6) && near(front.yaw, 0.0), "far ball lost approach speed");
        const auto back = chaseVelocity(-1.0, 0.0, 1.6, 1.5);
        check(near(back.x, 0.0) && back.yaw > 0.0, "rear target makes robot walk away while turning");
        const auto side = chaseVelocity(0.0, 1.0, 1.6, 1.5);
        check(std::abs(side.x) < 1e-8 && side.yaw > 0.0, "side target requires turning first");
        const auto arrived = chaseVelocity(0.04, 0.0, 1.6, 1.5);
        check(near(arrived.x, 0.0) && near(arrived.yaw, 0.0), "target arrival still walks through the ball");
        check(chaseVelocity(0.4, 0.0, 1.6, 1.5).x < front.x, "approach fails to slow near target");
        check(near(chaseVelocity(1.0, 0.0, 0.0, 1.5).x, 0.0), "shared-ball stand-off is ignored");
        check(near(chaseVelocity(std::numeric_limits<double>::quiet_NaN(), 0, 1.6, 1.5).x, 0.0),
              "invalid chase target generates motion");

        const double soft = dribbleSpeedLimit(1.4, true, true, 0.3);
        check(near(soft, 0.3), "soft kickoff exceeds configured speed");
        double speed = 0.3;
        for (int i = 0; i < 500; ++i) {
            speed = rampDribbleSpeed(speed, soft, 10.0);
            check(speed <= 0.3, "soft kickoff accelerates past its limit after first tick");
        }
        check(near(dribbleSpeedLimit(0.2, true, true, 0.3), 0.2), "soft kickoff increases a smaller request");
        check(near(dribbleSpeedLimit(1.4, true, false, 0.3), 1.4), "normal dribble is incorrectly slowed");
        check(near(rampDribbleSpeed(0.3, 1.4, 10), 0.308), "dribble acceleration depends on tick count");
        check(near(rampDribbleSpeed(0.3, 1.4, 10000), 0.38), "long scheduling gap causes a speed jump");
        double fast = 0.3, slow = 0.3;
        for (int i = 0; i < 100; ++i) fast = rampDribbleSpeed(fast, 1.4, 10);
        for (int i = 0; i < 20; ++i) slow = rampDribbleSpeed(slow, 1.4, 50);
        check(near(fast, slow), "same elapsed time gives different dribble speeds");

        using namespace observation_geometry;
        const Point stationary{1.0, 0.2, 0.0};
        for (int i = 0; i < 400; ++i) {
            const Pose2D captured{0.002 * i, -0.001 * i, 0.004 * i};
            const Pose2D current{captured.x + 0.15, captured.y + 0.03, captured.theta + 0.3};
            const Point imagePoint = toLocal(stationary, captured);
            const Point odomPoint = toParent(imagePoint, captured);
            check(near(odomPoint.x, stationary.x) && near(odomPoint.y, stationary.y),
                  "camera latency makes stationary ball move in odom");
            const auto commandPoint = toLocal(odomPoint, current);
            const auto expected = toLocal(stationary, current);
            check(near(commandPoint.x, expected.x) && near(commandPoint.y, expected.y),
                  "motion still uses capture-time relative coordinates");
            const Pose2D recalibrated{-2.0, 0.4, 0.8};
            const auto fieldBall = toParent(odomPoint, recalibrated);
            const auto fieldRobot = toParent({current.x, current.y, 0.0}, recalibrated);
            const auto afterCalibration = toLocal(fieldBall,
                {fieldRobot.x, fieldRobot.y, current.theta + recalibrated.theta});
            check(near(afterCalibration.x, commandPoint.x) && near(afterCalibration.y, commandPoint.y),
                  "localization correction changes feet-side ball geometry");
        }

        // Chase from both sides, using coarse positioning before final
        // adjustment when distance is close but the approach line is wrong.
        for (const Pose2D start : {Pose2D{-3, -1, 1.2}, Pose2D{-3, 1, -1.2}}) {
            const auto result = runApproach(start, {}, 0.0, 0.2);
            check(result.ready, "chase/reposition/adjust loop cannot reach a kick stance");
            check(result.minRange >= 0.3, "chase approach walks through the ball");
        }

        // P3's recorded feet-side pose: 0.352 m range, 0.298 rad bearing,
        // 2.5335 rad route error. Mirror it and include the +/-pi ambiguity,
        // side-facing bodies and a small-error pose that should skip orbit.
        for (const double sign : {-1.0, 1.0}) {
            for (const double route : {2.5335, kick_geometry::kPi - 0.001}) {
                for (const double bearing : {0.298, 1.1}) {
                    const auto start = approachPose(0.352, sign * route, sign * bearing);
                    const auto result = runApproach(start, {}, 0.0, 0.2);
                    check(result.ready, "wrong-side feet-ball approach never reaches kick readiness");
                    check(result.steps < 2500, "wrong-side feet-ball approach keeps orbiting too long");
                    check(result.retreats > 0 && result.orbits > 0 && result.adjustments > 0,
                          "wrong-side feet-ball approach skips retreat, orbit or fine adjustment");
                    check(result.minRange >= 0.3,
                          "wrong-side approach crosses the safe ball radius");
                }
            }
            const auto fine = runApproach(approachPose(0.4, sign * 0.5, sign * 0.1), {}, 0.0, 0.2);
            check(fine.ready && fine.orbits == 0,
                  "small route error unnecessarily starts coarse positioning");
        }

        // A translated/rotated field must give the same recovery; body-local
        // commands cannot assume the ball is at the origin or the goal at +x.
        const Point displacedBall{2.5, -1.2, 0.0};
        const auto displaced = runApproach(
            approachPose(0.352, 2.5335, 0.298, 1.1, displacedBall), displacedBall, 1.1, 0.2);
        check(displaced.ready && displaced.minRange >= 0.3,
              "rotated field geometry prevents safe kick approach");
        const auto reference = runApproach(approachPose(0.352, 2.5335, 0.298), {}, 0.0, 0.2);
        check(displaced.steps == reference.steps,
              "field coordinate transform changes the local approach trajectory");

        // With no actuator dead zone the same pipeline must still settle.
        const auto ideal = runApproach(approachPose(0.352, -2.5335, -0.298));
        check(ideal.ready && ideal.minRange >= 0.3,
              "ideal actuator cannot finish safe coarse-to-fine alignment");
        std::cout << "Motion: chase, dribble ramp, delayed observations and kick approach passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
