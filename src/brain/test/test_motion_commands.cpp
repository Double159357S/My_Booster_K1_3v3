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

        // Chase a ball from both sides with the shipped 1 m chase/adjust gate.
        for (const Pose2D start : {Pose2D{-3, -1, 1.2}, Pose2D{-3, 1, -1.2}}) {
            Pose2D robot = start;
            bool ready = false;
            for (int step = 0; step < 4000; ++step) {
                const auto ball = toLocal({0, 0, 0}, robot);
                const double range = std::hypot(ball.x, ball.y);
                const double bearing = std::atan2(ball.y, ball.x);
                if (range <= 0.5 && alignedForKick(ball.x, ball.y, robot.theta, 0.0, AlignmentLimits{})) {
                    ready = true;
                    break;
                }
                double vx, vy = 0.0, yaw;
                if (range > 1.0) {
                    const auto target = toLocal({-0.1, 0, 0}, robot);
                    const auto command = chaseVelocity(target.x, target.y, 1.6, 1.5);
                    vx = command.x;
                    yaw = command.yaw;
                } else {
                    const double error = kick_geometry::normalizeAngle(-std::atan2(-robot.y, -robot.x));
                    const auto command = adjustmentVelocity(range, bearing, error, 0.4, 0.15, 0.5);
                    vx = command.x;
                    vy = command.y;
                    yaw = adjustmentTurnVelocity(bearing, error, -robot.theta, 0.5, 1.5, 0.1);
                }
                robot.x += (vx * std::cos(robot.theta) - vy * std::sin(robot.theta)) * 0.01;
                robot.y += (vx * std::sin(robot.theta) + vy * std::cos(robot.theta)) * 0.01;
                robot.theta = kick_geometry::normalizeAngle(robot.theta + yaw * 0.01);
            }
            check(ready, "chase/adjust loop cannot reach a kick stance");
        }
        std::cout << "Motion: chase, dribble ramp, delayed observations and kick approach passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
