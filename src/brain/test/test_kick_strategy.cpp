#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "utils/kick_strategy.h"

namespace {
bool check(const std::string &name, bool condition) {
    if (!condition) std::cerr << name << " failed\n";
    return condition;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
}

int main() {
    using namespace kick_strategy;
    bool ok = true;
    AlignmentLimits limits;
    ok &= check("aligned close ball", alignedForKick(0.6, 0.0, 0.0, 0.0, limits));
    ok &= check("far ball requires chase", !alignedForKick(1.6, 0.0, 0.0, 0.0, limits));
    ok &= check("side ball requires alignment", !alignedForKick(0.6, 0.4, 0.0, 0.0, limits));
    ok &= check("approach line misses target", !alignedForKick(0.6, 0.2, 0.0, 0.0, limits));
    ok &= check("behind body", !alignedForKick(-0.6, 0.0, 0.0, 0.0, limits));
    ok &= check("body false detection", !alignedForKick(0.02, 0.0, 0.0, 0.0, limits));
    ok &= check("invalid ball", !alignedForKick(
        std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0, limits));
    limits.bearing = 1.0;
    ok &= check("good approach alone is insufficient", !alignedForKick(
        0.6 * std::cos(0.6), -0.6 * std::sin(0.6), 0.6, 0.0, limits));
    limits.bearing = 0.35;
    ok &= check("heading wrap", alignedForKick(0.6, 0.0,
        kick_geometry::kPi - 0.05, -kick_geometry::kPi + 0.05, limits));

    ok &= check("SDK can handle remaining small aiming error",
        alignedForKick(0.4, 0.0, 0.15, 0.0, limits));
    ok &= check("entry threshold still rejects body misalignment",
        !alignedForKick(0.4, 0.0, 0.27, 0.0, limits));
    const auto starting = startupAlignmentLimits(limits);
    ok &= check("small settling change does not cancel SDK start",
        alignedForKick(0.4, 0.0, 0.27, 0.0, starting));
    ok &= check("large settling change cancels SDK start",
        !alignedForKick(0.4, 0.0, 0.6, 0.0, starting));

    BallObservation observation;
    observation.known = true;
    observation.detected = true;
    observation.x = 0.3;
    const auto select = [&]() { return selectBallSource(observation, 650.0, 1000.0, 0.8, true, true); };
    ok &= check("visible feet-side ball", select() == BallSource::real);
    observation.detected = false;
    observation.ageMs = 150.0;
    ok &= check("one missed frame still supplies a kick target", select() == BallSource::recentReal);
    observation.ageMs = 480.0;
    ok &= check("recent ball survives startup and head movement", select() == BallSource::recentReal);
    observation.ageMs = 651.0;
    ok &= check("old feet-side memory cannot continue kicking", select() == BallSource::none);
    observation.detected = true;
    ok &= check("stalled camera detection flag cannot make old ball fresh", select() == BallSource::none);
    observation.detected = false;
    observation.predictionReliable = true;
    observation.predictedX = 0.3;
    ok &= check("old feet-side prediction also rejected", select() == BallSource::none);
    observation.ageMs = 100.0;
    ok &= check("disabled memory respects configuration", selectBallSource(
        observation, 650.0, 1000.0, 0.8, true, false) == BallSource::none);
    observation.known = false;
    ok &= check("unknown ball cannot start through recent memory", select() == BallSource::none);
    observation.known = true;
    observation.lost = true;
    ok &= check("declared ball loss cancels the kick", select() == BallSource::none);
    observation.lost = false;
    observation.x = 2.0;
    observation.predictedX = 2.1;
    observation.ageMs = 800.0;
    ok &= check("recent distant prediction remains usable", select() == BallSource::predicted);
    observation.predictionReliable = false;
    ok &= check("recent distant memory remains usable", select() == BallSource::memory);
    observation.ageMs = 1001.0;
    ok &= check("expired distant memory is rejected", select() == BallSource::none);
    observation.ageMs = -1.0;
    ok &= check("future timestamp is rejected", select() == BallSource::none);
    observation.ageMs = 0.0;
    observation.detected = true;
    observation.x = std::numeric_limits<double>::quiet_NaN();
    ok &= check("invalid detection cannot provide a target", select() == BallSource::none);
    observation.x = observation.y = 0.0;
    ok &= check("placeholder detection cannot provide a target", select() == BallSource::none);

    const auto claim = [](double x, double y, double range) {
        return goalieCanClaim(x, y, range, 9.0, 2.0, 5.0, 0.3, 2.2);
    };
    ok &= check("goalie approaches ball in front of old cutoff", claim(-2.4, 0.0, 1.8));
    ok &= check("goalie keeps normal close clearance", claim(-3.0, 0.5, 0.9));
    ok &= check("goalie does not chase midfield", !claim(-1.0, 0.0, 2.0));
    ok &= check("goalie does not chase far ball", !claim(-2.4, 0.0, 2.4));
    ok &= check("goalie does not chase beyond touchline", !claim(-3.0, 3.0, 1.0));
    ok &= check("goalie does not chase behind goal", !claim(-4.8, 0.0, 1.0));
    ok &= check("clearance area follows field dimensions",
        inGoalieClearanceArea(-6.0, 0.0, 22.0, 6.0, 10.0, 0.3));

    ok &= check("no ownership means defend",
        clearanceAction(false, 0.6, 1.2, true, true) == ClearanceAction::defend);
    ok &= check("distant owned ball means approach",
        clearanceAction(true, 1.8, 1.2, true, true) == ClearanceAction::approach);
    ok &= check("close misaligned ball means align",
        clearanceAction(true, 0.9, 1.2, false, true) == ClearanceAction::align);
    ok &= check("cooldown allows adjustment instead of retreat",
        clearanceAction(true, 0.9, 1.2, true, false) == ClearanceAction::align);
    ok &= check("aligned clearance starts kick",
        clearanceAction(true, 0.9, 1.2, true, true) == ClearanceAction::kick);
    ok &= check("placeholder ball cannot start kick",
        clearanceAction(true, 0.0, 1.2, true, true) == ClearanceAction::defend);

    const auto settled = adjustmentVelocity(0.4, 0.0, 0.0, 0.4, 0.15, 0.5);
    ok &= check("aligned stance has no lateral drift", near(settled.x, 0.0) && near(settled.y, 0.0));
    const auto approaching = adjustmentVelocity(1.2, 0.0, 0.0, 0.4, 0.15, 0.5);
    ok &= check("aligned approach has no sideways kick", near(approaching.x, 0.5) && near(approaching.y, 0.0));
    const auto close = adjustmentVelocity(0.2, 0.0, 0.0, 0.4, 0.15, 0.5);
    ok &= check("too close ball permits effective retreat", near(close.x, -0.3));
    const auto positive = adjustmentVelocity(0.4, 0.0, 0.01, 0.4, 0.15, 0.5);
    ok &= check("alignment noise does not cause constant orbiting", near(positive.y, 0.0));
    const auto correcting = adjustmentVelocity(0.4, 0.0, 0.14, 0.4, 0.15, 0.5);
    const auto negative = adjustmentVelocity(0.4, 0.0, -0.14, 0.4, 0.15, 0.5);
    ok &= check("remaining approach error gets an effective lateral command", near(correcting.y, -0.3));
    ok &= check("effective adjustment mirrors with error sign", near(correcting.y, -negative.y));
    ok &= check("remaining heading error gets an effective turn", near(
        adjustmentTurnVelocity(0.0, 0.0, 0.08, 0.5, 1.5, 0.1), 0.3));
    ok &= check("aligned body stops turning", near(
        adjustmentTurnVelocity(0.0, 0.0, 0.0, 0.5, 1.5, 0.1), 0.0));
    ok &= check("aligned approach still corrects body heading", near(
        adjustmentHeadingError(0.0, 0.0, -0.08, 0.5), -0.08));
    ok &= check("large orbit error keeps ball in view", near(
        adjustmentHeadingError(0.4, 1.2, -1.0, 0.5), 0.4));

    // A simple planar trajectory must settle on the shot line, rather than
    // orbit forever with a constant lateral speed after alignment.
    double x = -0.8, y = -0.5, heading = 0.4;
    bool converged = false;
    for (int i = 0; i < 1600; ++i) {
        const double bx = -x * std::cos(heading) - y * std::sin(heading);
        const double by = x * std::sin(heading) - y * std::cos(heading);
        const double range = std::hypot(bx, by);
        const double bearing = std::atan2(by, bx);
        const double error = -std::atan2(-y, -x);
        const auto command = adjustmentVelocity(range, bearing, error, 0.4, 0.15, 0.5);
        x += (command.x * std::cos(heading) - command.y * std::sin(heading)) * 0.01;
        y += (command.x * std::sin(heading) + command.y * std::cos(heading)) * 0.01;
        heading += adjustmentTurnVelocity(bearing, error, -heading, 0.5, 1.5, 0.1) * 0.01;
        if (alignedForKick(bx, by, heading, 0.0, limits) && range < 0.5) {
            converged = true;
            break;
        }
    }
    ok &= check("adjustment converges to kick stance", converged);

    // Reproduce the feet-side stall with a walking controller that ignores
    // commands below 0.2. Small proportional commands alone cannot converge.
    // Even a tolerance tighter than the production entry threshold must be
    // reachable; relaxing entry alone should not mask a stalled adjustment.
    limits.approachError = limits.bodyError = 0.1125;
    x = -0.4 * std::cos(0.14);
    y = -0.4 * std::sin(0.14);
    heading = -0.12;
    converged = false;
    const auto actuator = [](double value) {
        return std::abs(value) < 0.2 ? 0.0 : value;
    };
    for (int i = 0; i < 1000; ++i) {
        const double bx = -x * std::cos(heading) - y * std::sin(heading);
        const double by = x * std::sin(heading) - y * std::cos(heading);
        if (alignedForKick(bx, by, heading, 0.0, limits)) {
            converged = true;
            break;
        }
        const double range = std::hypot(bx, by);
        const double bearing = std::atan2(by, bx);
        const double error = -std::atan2(-y, -x);
        const auto command = adjustmentVelocity(range, bearing, error, 0.4, 0.15, 0.5);
        const double vx = actuator(command.x), vy = actuator(command.y);
        x += (vx * std::cos(heading) - vy * std::sin(heading)) * 0.01;
        y += (vx * std::sin(heading) + vy * std::cos(heading)) * 0.01;
        heading += actuator(adjustmentTurnVelocity(bearing, error, -heading, 0.5, 1.5, 0.1)) * 0.01;
    }
    ok &= check("feet-side adjustment overcomes actuator dead zone", converged);
    return ok ? 0 : 1;
}
