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

    ok &= check("active SDK keeps its slot during foot alignment and missed frames",
        keepVisualKickSession(true, true, true, false));
    ok &= check("new kicks must still pass entry checks",
        !keepVisualKickSession(false, true, true, false));
    ok &= check("revoked ownership interrupts the SDK",
        !keepVisualKickSession(true, true, false, false));
    ok &= check("ball out interrupts the SDK",
        !keepVisualKickSession(true, true, true, true));
    ok &= check("finished SDK does not revive an unrelated decision",
        !keepVisualKickSession(true, false, true, false));

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
    ok &= check("aligned keeper waits without retreating or orbiting during cooldown",
        clearanceAction(true, 0.9, 1.2, true, false) == ClearanceAction::wait);
    ok &= check("misaligned keeper can still adjust during cooldown",
        clearanceAction(true, 0.9, 1.2, false, false) == ClearanceAction::align);
    ok &= check("shared ball still requires local confirmation before cooldown or kick",
        clearanceAction(true, 0.4, 1.2, true, true, false) == ClearanceAction::approach);
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
    const auto bodyOnly = adjustmentVelocity(0.4, 0.0, 0.14, 0.4, 0.15, 0.5, 0.25);
    ok &= check("accepted approach line stays fixed while body turns", near(bodyOnly.y, 0.0));
    const auto keeperBodyOnly = adjustmentVelocity(0.4, 0.0, 0.4, 0.4, 0.22, 0.3, 0.55);
    ok &= check("keeper does not keep orbiting an accepted clearance line", near(keeperBodyOnly.y, 0.0));
    const auto outsideLine = adjustmentVelocity(0.4, 0.0, 0.7, 0.4, 0.22, 0.3, 0.55);
    ok &= check("keeper still orbits a genuinely wrong approach line", outsideLine.y < 0.0);

    // A tiny component must not become a full independent walking step. The
    // historical per-axis floor turned this almost lateral orbit into a
    // diagonal (0.3, -0.3) command and overshot the stance repeatedly.
    const auto acceptedLine = adjustmentVelocity(0.4, 0.1, 0.24, 0.4, 0.15, 0.5, 0.25);
    const auto justOutsideLine = adjustmentVelocity(0.4, 0.1, 0.26, 0.4, 0.15, 0.5, 0.25);
    ok &= check("accepted shot line has no unnecessary orbit",
        near(acceptedLine.x, 0.0) && near(acceptedLine.y, 0.0));
    ok &= check("minimum adjustment speed preserves vector direction",
        near(std::hypot(justOutsideLine.x, justOutsideLine.y), 0.3) &&
        near(justOutsideLine.x / justOutsideLine.y, -std::tan(0.1)));
    ok &= check("tiny forward adjustment is not promoted to a full step",
        justOutsideLine.x > 0.0 && justOutsideLine.x < 0.04 &&
        justOutsideLine.y < -0.29);
    const auto oppositeLine = adjustmentVelocity(0.4, -0.1, -0.26, 0.4, 0.15, 0.5, 0.25);
    ok &= check("vector adjustment mirrors across the shot line",
        near(oppositeLine.x, justOutsideLine.x) && near(oppositeLine.y, -justOutsideLine.y));
    const auto fullVector = effectiveAdjustmentVector(0.4, -0.3);
    ok &= check("already effective vector keeps magnitude and direction",
        near(fullVector.x, 0.4) && near(fullVector.y, -0.3));
    ok &= check("nonfinite vector is stopped", near(effectiveAdjustmentVector(
        std::numeric_limits<double>::infinity(), 0.0).x, 0.0));

    ok &= check("near ball on wrong side still needs coarse positioning",
        needsReposition(0.352, 2.5335, false));
    ok &= check("coarse positioning enters outside the larger threshold",
        !needsReposition(0.4, 0.65, false) && needsReposition(0.4, 0.66, false));
    ok &= check("coarse positioning hysteresis holds through middle band",
        !needsReposition(0.4, 0.4, false) && needsReposition(0.4, 0.4, true));
    ok &= check("coarse positioning exits at the smaller threshold",
        !needsReposition(0.4, 0.35, true) && needsReposition(0.4, 0.36, true));
    ok &= check("coarse positioning uses shortest wrapped angle",
        needsReposition(0.4, 0.7 + 2.0 * kick_geometry::kPi, false) &&
        needsReposition(0.4, -0.7 - 2.0 * kick_geometry::kPi, false) &&
        !needsReposition(0.4, 0.3 + 2.0 * kick_geometry::kPi, true));
    ok &= check("invalid ball or angle cannot request a coarse orbit",
        !needsReposition(0.0, 2.0, false) &&
        !needsReposition(std::numeric_limits<double>::infinity(), 2.0, false) &&
        !needsReposition(0.4, std::numeric_limits<double>::quiet_NaN(), true));

    const auto retreat = repositionVelocity(0.352, 0.298, 2.5335, 0.55, 0.45, 1.5);
    const double retreatRadial = retreat.x * std::cos(0.298) + retreat.y * std::sin(0.298);
    const double retreatTangent = -retreat.x * std::sin(0.298) + retreat.y * std::cos(0.298);
    ok &= check("feet-side coarse positioning retreats before orbiting",
        retreat.retreat && retreatRadial < -0.29 && near(retreatTangent, 0.0));
    ok &= check("coarse retreat stays inside speed and turn limits",
        std::hypot(retreat.x, retreat.y) <= 0.45 && std::abs(retreat.yaw) <= 1.5);
    for (const double sign : {-1.0, 1.0}) {
        const double bearing = sign * 0.2;
        const auto orbit = repositionVelocity(0.55, bearing, sign * 2.5, 0.55, 0.45, 1.5);
        const double radial = orbit.x * std::cos(bearing) + orbit.y * std::sin(bearing);
        const double tangent = -orbit.x * std::sin(bearing) + orbit.y * std::cos(bearing);
        ok &= check("coarse orbit maintains stand-off on either side",
            !orbit.retreat && near(radial, 0.0) && sign * tangent < -0.3);
        ok &= check("coarse orbit respects speed limit", std::hypot(orbit.x, orbit.y) <= 0.45);
    }
    const auto turnFirst = repositionVelocity(0.352, 0.8, 2.5, 0.55, 0.45, 1.5);
    ok &= check("side ball is brought into view before translating",
        near(turnFirst.x, 0.0) && near(turnFirst.y, 0.0) && turnFirst.yaw > 0.0);
    const auto latchedPositive = repositionVelocity(0.55, 0.0,
        -kick_geometry::kPi + 0.01, 0.55, 0.45, 1.5, 1);
    const auto latchedNegative = repositionVelocity(0.55, 0.0,
        kick_geometry::kPi - 0.01, 0.55, 0.45, 1.5, -1);
    ok &= check("latched orbit direction survives angle wrapping",
        latchedPositive.y < 0.0 && latchedNegative.y > 0.0);
    const auto invalidOrbit = repositionVelocity(0.4, 0.0,
        std::numeric_limits<double>::quiet_NaN(), 0.55, 0.45, 1.5);
    ok &= check("invalid orbit target stops every motion axis",
        near(invalidOrbit.x, 0.0) && near(invalidOrbit.y, 0.0) && near(invalidOrbit.yaw, 0.0));

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
        const auto command = adjustmentVelocity(range, bearing, error, 0.4, 0.15, 0.5, 0.25);
        x += (command.x * std::cos(heading) - command.y * std::sin(heading)) * 0.01;
        y += (command.x * std::sin(heading) + command.y * std::cos(heading)) * 0.01;
        heading += adjustmentTurnVelocity(bearing, error, -heading, 0.25, 1.5, 0.1) * 0.01;
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
        heading += actuator(adjustmentTurnVelocity(bearing, error, -heading, 0.25, 1.5, 0.1)) * 0.01;
    }
    ok &= check("feet-side adjustment overcomes actuator dead zone", converged);
    return ok ? 0 : 1;
}
