// ROS/transport boundaries only. Node declarations, functions, JSON encoding,
// response matching and request lifecycle come from the shipped source.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>
#include "utils/geometry_types.h"
#include "utils/kick_strategy.h"
#include "utils/team_ball.h"
#include "utils/visual_kick_request.h"
#include "booster_interface/third_party/nlohmann_json/json.hpp"
using namespace std;
constexpr int RCL_ROS_TIME = 1;
#ifndef M_PI
constexpr double M_PI = kick_geometry::kPi;
#endif
constexpr int HL_MAX_NUM_PLAYERS = 5;
constexpr int64_t kApiIdEnableVisualKickMode = 2038;
double monotonicMs = 10000.0;
namespace team_cooperation { double steadyNowMs() { return monotonicMs; } }
namespace rclcpp {
struct Duration { double ms; double seconds() const { return ms / 1000.0; } };
struct Time {
    double ms = 0.0;
    Time() = default;
    Time(int sec, int ns, int) : ms(sec * 1000.0 + ns / 1e6) {}
    int64_t nanoseconds() const { return static_cast<int64_t>(ms * 1e6); }
    friend Duration operator-(Time a, Time b) { return {a.ms - b.ms}; }
};
template<class T> struct Publisher { using SharedPtr = shared_ptr<Publisher>; vector<T> sent; void publish(T msg) { sent.push_back(msg); } };
template<class T> struct Subscription { using SharedPtr = shared_ptr<Subscription>; };
}
namespace booster_msgs::msg { struct RpcReqMsg { string uuid, header, body; }; using RpcRespMsg = RpcReqMsg; }
namespace booster_interface::msg { struct BoosterApiReqMsg { int64_t api_id = 0; string body; }; }
namespace rerun { struct TextLog { string text; explicit TextLog(string s) : text(std::move(s)) {} }; }
namespace rerun {
struct Vec2D { double x, y; };
struct Boxes2D {
    static Boxes2D from_mins_and_sizes(const vector<Vec2D> &, const vector<Vec2D> &) { return {}; }
    Boxes2D &with_labels(initializer_list<string>) { return *this; }
    Boxes2D &with_colors(int) { return *this; }
};
}
#define RCLCPP_INFO(...) ((void)0)
#define RCLCPP_WARN(...) ((void)0)
#define RCLCPP_INFO_THROTTLE(...) ((void)0)
string gen_uuid() { static int next = 0; return "offline-" + to_string(++next); }
string format(const char *message, ...) { return message; }
double cap(double value, double high, double low) { return clamp(value, low, high); }
double norm(double x, double y) { return hypot(x, y); }
double mean(double x, double y) { return (x + y) / 2.0; }
double toPInPI(double angle) { return kick_geometry::normalizeAngle(angle); }
enum class NodeStatus { SUCCESS, RUNNING };
struct NodeConfig {};
using PortsList = vector<int>;
template<class T, class... Args> int InputPort(const string &, Args &&...) { return 0; }
template<class T, class... Args> int OutputPort(const string &, Args &&...) { return 0; }
class NodeBase {
public:
    NodeBase(const string &, const NodeConfig &) {}
    virtual ~NodeBase() = default;
    virtual NodeStatus tick() { return NodeStatus::SUCCESS; }
    virtual NodeStatus onStart() { return NodeStatus::SUCCESS; }
    virtual NodeStatus onRunning() { return NodeStatus::SUCCESS; }
    virtual void onHalted() {}
    map<string, double> numbers{{"chase_threshold", 1.0}, {"min_msec_kick", 1500}, {"max_msec_kick", 7000}, {"range", 6.0},
        {"vx_limit", 0.6}, {"vy_limit", 0.4}, {"vtheta_limit", 1.5}, {"dist", 0.8}, {"safe_dist", 1.0},
        {"turn_threshold", 0.5}, {"vtheta_factor", 1.5}, {"tangential_speed_far", 0.7},
        {"tangential_speed_near", 0.15}, {"near_threshold", 0.8}, {"no_turn_threshold", 0.1},
        {"turn_first_threshold", 0.5}};
    map<string, string> strings{{"decision_in", ""}, {"position", "offense"}, {"mode", "chase"}};
    template<class T> optional<T> getInput(const string &key) const {
        if constexpr (is_same_v<T, string>) return strings.at(key);
        else return static_cast<T>(numbers.at(key));
    }
    template<class T> void getInput(const string &key, T &value) const { value = getInput<T>(key).value(); }
    template<class T> void setOutput(const string &key, T value) { strings[key] = value; }
};
using StatefulActionNode = NodeBase;
using SyncActionNode = NodeBase;
class Brain;
// CLIENT_DECLARATION
struct FakeClient : RobotClient {
    explicit FakeClient(Brain *b) : RobotClient(b) { publisher = make_shared<rclcpp::Publisher<booster_msgs::msg::RpcReqMsg>>(); }
    struct Command { double x = 0, y = 0, yaw = 0; } command;
    int moves = 0;
    vector<pair<double, double>> heads;
    int moveHead(double pitch, double yaw) { heads.push_back({pitch, yaw}); return 0; }
    int setVelocity(double x, double y, double yaw, bool = true, bool = true, bool = true) {
        command = {x, y, yaw}; ++moves; return 0;
    }
    int moveToPoseOnField3(double, double, double, double, double, double, double, double, double, double, double, bool) { ++moves; return 0; }
};
struct FakeTree {
    map<string, variant<bool, int, string>> entries{
        {"control_state", 3}, {"player_role", string("striker")}, {"gc_game_state", string("PLAY")},
        {"ball_location_known", true}, {"tm_ball_pos_reliable", false}, {"odom_calibrated", true},
        {"ball_out", false}, {"goalie_block_kick_active", false}, {"goalie_mode", string("guard_defend")},
        {"gc_is_kickoff_side", true}, {"gc_opponent_set_play_search_direction", 1}};
    template<class T> T getEntry(const string &key) const {
        const auto it = entries.find(key); return it == entries.end() ? T{} : get<T>(it->second);
    }
    template<class T> void setEntry(const string &key, T value) { entries[key] = value; }
};
struct FakeBall {
    Point posToRobot{0.4, 0, 0}, posToField{0, 0, 0}, reliablePredictPosToRobot, reliablePredictPosToField;
    double range = 0.4, yawToRobot = 0.0;
    double pitchToRobot = 0.9;
    struct { double xmin = 300, xmax = 340, ymin = 220, ymax = 260; } boundingBox;
    rclcpp::Time timePoint;
};
struct FakePeer { bool isAlive = false, isInVisualKick = false; double cost = 0; };
struct FakeData {
    FakeBall ball;
    Pose2D robotPoseToField;
    bool tmImLead = true, ballDetected = true, ballFromTeammate = false, lose_ball = false;
    bool tmImInVisualKick = false, tmImInDribbleKick = false, shouldExitRLVisionKick = false;
    bool isKickingOff = false, isOpponentKickingOff = false, isFreekickKickingOff = false;
    bool kickoffStrikerGroupLatched = false, goalieVisualKickTargetValid = false;
    int kickoffStrikerGroupRank = 0, tmMyStrikerCostRank = 0, kickoffBallHalf = 0;
    double kickDir = 0.0, robotBallAngleToField = 0.0, tmMyCost = 1.0;
    double headPitch = 0.9, headYaw = 0.0;
    FakeBall tmBall;
    team_ball::Recovery sharedBallRecovery;
    team_ball::RecoverySettings sharedBallRecoverySettings;
    string kickType = "shoot", realGameSubState = "CORNER_KICK", goalieVisualKickSubState = "none";
    Point goalieVisualKickTargetField;
    vector<FakePeer> tmStatus = vector<FakePeer>(5);
    uint64_t setPlaySearchEpoch = 1;
    Pose2D field2robot(const Pose2D &pose) const {
        const double dx = pose.x - robotPoseToField.x, dy = pose.y - robotPoseToField.y;
        return {dx * cos(robotPoseToField.theta) + dy * sin(robotPoseToField.theta),
                -dx * sin(robotPoseToField.theta) + dy * cos(robotPoseToField.theta),
                toPInPI(pose.theta - robotPoseToField.theta)};
    }
};
struct FakeConfig {
    int numOfPlayers = 3, playerId = 1;
    bool enableCom = true;
    bool limitNearBallSpeed = false;
    double nearBallRange = 1.0, nearBallSpeedLimit = 0.3;
    double camPixX = 640, camPixY = 480, camAngleX = 1.0, camAngleY = 0.8;
    string RLVisionKickVisualKickVersion = "kV2";
    struct { double length = 9, width = 6, penaltyAreaLength = 2, goalAreaLength = 1; } fieldDimensions;
};
struct FakeLog {
    vector<string> messages;
    void setTimeNow() {}
    void log(const string &, rerun::TextLog text) { messages.push_back(text.text); }
    void log(const string &, const rerun::Boxes2D &) {}
    template<class... Args> void logToScreen(Args &&...) {}
};
struct FakeParameter {
    double value = 0;
    double as_double() const { return value; }
    bool as_bool() const { return value != 0; }
};
struct FakeClock { rclcpp::Time time; rclcpp::Time now() const { return time; } };
class Brain {
public:
    FakeTree treeStorage; FakeTree *tree = &treeStorage;
    FakeData dataStorage; FakeData *data = &dataStorage;
    FakeConfig configStorage; FakeConfig *config = &configStorage;
    FakeLog logStorage; FakeLog *log = &logStorage;
    FakeClock clock;
    FakeClient clientStorage{this}; FakeClient *client = &clientStorage;
    bool blocked = false, angleGood = true;
    bool blockAllDirections = false;
    int blockedOrbitSide = 0;
    kick_strategy::BallSource source = kick_strategy::BallSource::real;
    // Explicit legacy tuning keeps the precise-alignment and fallback tests
    // useful. fastKickFlow below exercises the actual shipped match settings.
    map<string, double> parameters{
        {"strategy.enable_auto_visual_kick", 1}, {"strategy.auto_visual_kick_enable_dist_min", 0.05},
        {"strategy.auto_visual_kick_enable_dist_max", 1.4}, {"strategy.auto_visual_kick_enable_angle", 0.35},
        {"strategy.auto_visual_kick_max_direction_error", 0.25}, {"strategy.auto_visual_kick_max_body_error", 0.25},
        {"strategy.auto_visual_kick.cooldown_ms", 600}, {"strategy.auto_visual_kick.start_retry_ms", 1000},
        {"strategy.auto_visual_kick.start_ack_timeout_ms", 1200}, {"strategy.auto_visual_kick.startup_decelerate_ms", 160},
        {"strategy.auto_visual_kick.exit_decelerate_ms", 140}, {"strategy.auto_visual_kick.progress_range_delta", 0.12},
        {"strategy.auto_visual_kick.progress_ball_forward_delta", 0.15}, {"strategy.auto_visual_kick.no_progress_timeout_ms", 3500},
        {"strategy.auto_visual_kick.max_total_ms", 14000}, {"strategy.auto_visual_kick.max_parallel_count", 1},
        {"strategy.auto_visual_kick.max_extra_rounds", 1}, {"strategy.auto_visual_kick.yield_my_cost_min", 4},
        {"strategy.kick_range", 1.0}, {"strategy.kick_theta_range", 0.2}, {"strategy.shoot.threat_threshold", -2},
        {"strategy.kick_approach.reposition_enter_angle", 0.65},
        {"strategy.kick_approach.reposition_exit_angle", 0.35},
        {"strategy.kick_approach.orbit_distance", 0.55}, {"strategy.kick_approach.orbit_speed", 0.45},
        {"strategy.far_set_play_search.trigger_x", 0.5}, {"strategy.far_set_play_search.field_target_x_ratio", 0.25},
        {"strategy.far_set_play_search.lane_y_ratio", 0.28}, {"strategy.far_set_play_search.max_move_secs", 30},
        {"strategy.far_set_play_search.vx_limit", 0.7}, {"strategy.far_set_play_search.vy_limit", 0.4}};
    Brain() { monotonicMs += 5000; clock.time.ms = monotonicMs; data->ball.timePoint = clock.time; }
    FakeClock *get_clock() { return &clock; }
    double msecsSince(rclcpp::Time time) const { return clock.time.ms - time.ms; }
    FakeParameter get_parameter(const string &key) const {
        const auto it = parameters.find(key); return {it == parameters.end() ? 0.0 : it->second};
    }
    template<class T> void get_parameter(const string &key, T &value) const { value = static_cast<T>(get_parameter(key).value); }
    auto getKickBallSource() const { return source; }
    bool teammateBlocksKick() const { return blocked; }
    double teammateDistance() const { return blocked ? 0.4 : 100.0; }
    bool isAngleGood(double, const string &) const { return angleGood; }
    bool isAngleGoodForDirectionalKick(double) const { return true; }
    double distToObstacle(double direction) const {
        return blockAllDirections || blockedOrbitSide * sin(direction) > 0.1 ? 0.2 : 100.0;
    }
    bool isBallOut(double, double) const { return tree->getEntry<bool>("ball_out"); }
    double calcAvoidDir(double direction, double) const { return direction; }
    double kickValue(double) const { return 0.0; }
    double threatLevel() const { return 0.0; }
};
// TREE_DECLARATIONS
rclcpp::Time RLVisionKick::_lastExitTime(0, 0, RCL_ROS_TIME);
rclcpp::Time RLVisionKick::_lastStartFailureTime(0, 0, RCL_ROS_TIME);
// PRODUCTION_FUNCTIONS

void check(bool condition, const char *message) { if (!condition) throw runtime_error(message); }
void advanceClock(Brain &b, double ms) { b.clock.time.ms += ms; monotonicMs = b.clock.time.ms; b.data->ball.timePoint = b.clock.time; }
void respond(Brain &b, const string &uuid, const string &header) { b.client->visualKickResponse({uuid, header, ""}); }
string sentStart(const Brain &b) { return b.client->publisher->sent.back().uuid; }
bool cancelled(const Brain &b) {
    return !b.client->publisher->sent.empty() &&
        !nlohmann::json::parse(b.client->publisher->sent.back().body)["start"].get<bool>();
}
void request(Brain &b, RLVisionKick &action) {
    check(action.onStart() == NodeStatus::RUNNING, "aligned visual kick did not enter preparation");
    advanceClock(b, 161); action.onRunning();
    check(b.client->visualKickStartState() == kick_strategy::StartState::pending,
          "sending a request falsely marks it accepted");
    const auto header = nlohmann::json::parse(b.client->publisher->sent.back().header);
    check(header["api_id"] == 2038 && header["expect_response"] == true, "visual start did not request an RPC response");
}

// Feed the cooperation helper at the same boundary used by handleCooperation;
// head, motion and routing assertions below execute the extracted shipped nodes.
void sharedReport(Brain &b, double range, double bearing, bool owner, bool reliable = true) {
    b.data->ballDetected = false;
    b.data->ballFromTeammate = true;
    b.source = kick_strategy::BallSource::memory;
    b.tree->setEntry("ball_location_known", true);
    b.tree->setEntry("tm_ball_pos_reliable", reliable);
    b.data->ball.range = range;
    b.data->ball.yawToRobot = bearing;
    b.data->ball.posToRobot = {range * cos(bearing), range * sin(bearing), 0.0};
    b.data->ball.posToField = {1.0, 0.0, 0.0};
    b.data->tmBall = b.data->ball;
    const bool eligible = b.data->sharedBallRecovery.canPursue(
        monotonicMs, true, owner, range, 1.0, 0.0, bearing, reliable,
        b.data->sharedBallRecoverySettings);
    b.data->tmImLead = owner && eligible && reliable;
    b.data->sharedBallRecovery.syncOwner(monotonicMs, b.data->tmImLead, range, bearing,
                                       b.data->sharedBallRecoverySettings);
}

bool bodyStopped(const Brain &b) {
    const auto &command = b.client->command;
    return command.x == 0.0 && command.y == 0.0 && command.yaw == 0.0;
}

void localBall(Brain &b, double range, double bearing, double routeError) {
    b.data->ballDetected = true;
    b.data->ballFromTeammate = false;
    b.source = kick_strategy::BallSource::real;
    b.tree->setEntry("ball_location_known", true);
    b.tree->setEntry("tm_ball_pos_reliable", false);
    b.data->ball.range = range;
    b.data->ball.yawToRobot = bearing;
    b.data->ball.posToRobot = {range * cos(bearing), range * sin(bearing), 0.0};
    b.data->robotBallAngleToField = toPInPI(b.data->robotPoseToField.theta + bearing);
    b.data->ball.posToField = {
        b.data->robotPoseToField.x + range * cos(b.data->robotBallAngleToField),
        b.data->robotPoseToField.y + range * sin(b.data->robotBallAngleToField), 0.0};
    b.data->kickDir = toPInPI(b.data->robotBallAngleToField + routeError);
    b.data->ball.timePoint = b.clock.time;
}

string runDecision(Brain &b, StrikerDecide &decide, const string &previous) {
    decide.strings["decision_in"] = previous;
    b.tree->setEntry("decision", previous);
    decide.tick();
    const auto decision = decide.strings.at("decision_out");
    b.tree->setEntry("decision", decision);
    return decision;
}

void runChase(Brain &b, Chase &chase, const string &decision) {
    // Model the XML blackboard bindings explicitly; NodeBase only stores ports.
    b.tree->setEntry("decision", decision);
    chase.strings["mode"] = decision;
    chase.tick();
}

void useMatchKickParameters(Brain &b) {
    const map<string, double> settings{
        // MATCH_KICK_PARAMETERS
    };
    for (const auto &[key, value] : settings) b.parameters[key] = value;
}

void fastKickFlow() {
    // Both bearings and route/body errors exceed the former entry thresholds.
    // An existing orbit must yield to a kick on the very first decision tick.
    for (double sign : {-1.0, 1.0}) {
        Brain quick; useMatchKickParameters(quick);
        quick.angleGood = false;
        localBall(quick, 0.7, sign * 0.5, sign * 0.2);
        StrikerDecide decide("decide", {}, &quick);
        check(runDecision(quick, decide, "reposition") == "auto_visual_kick",
              "moderate alignment error still waits for orbit/fine adjustment");
        RLVisionKick action("visual", {}, &quick);
        const double detectedAt = quick.clock.time.ms;
        check(action.onStart() == NodeStatus::RUNNING, "priority decision rejected by action entry");
        advanceClock(quick, 30); action.onRunning();
        check(quick.client->publisher->sent.empty(), "startup skipped configured settling interval");
        advanceClock(quick, 10); action.onRunning();
        check(quick.client->visualKickStartState() == kick_strategy::StartState::pending &&
              quick.clock.time.ms - detectedAt <= 40,
              "match kick request takes longer than 40 ms after decision");
        action.onRunning();
        check(quick.client->publisher->sent.size() == 1, "pending start is sent repeatedly");
        respond(quick, sentStart(quick), "{\"status\":0}"); action.onRunning();
        action.onHalted();
        advanceClock(quick, 199);
        check(runDecision(quick, decide, "adjust") == "kick_wait", "cooldown is bypassed");
        advanceClock(quick, 1);
        check(runDecision(quick, decide, "adjust") == "auto_visual_kick",
              "match kick still waits the old 600 ms cooldown");
    }

    Brain direction; useMatchKickParameters(direction); direction.angleGood = false;
    localBall(direction, 0.8, 0.0, 0.75);
    StrikerDecide direct("decide", {}, &direction);
    check(runDecision(direction, direct, "reposition") == "auto_visual_kick",
          "route/body tolerance is still capped below the configured fast threshold");
    localBall(direction, 0.4, 0.2, 0.7);
    Adjust remainingTurn("adjust", {}, &direction); remainingTurn.numbers["range"] = 0.4;
    remainingTurn.tick();
    check(direction.client->command.x == 0.0 && direction.client->command.y == 0.0 &&
          direction.client->command.yaw > 0.0,
          "accepted approach line keeps orbiting while correcting the remaining body error");

    // These are valid in-field positions that the old adult-field constants
    // rejected. Ownership already accounts for cost; do not check it again.
    for (const Point pose : {Point{-10.0, 0.0, 0.0}, Point{0.0, 5.5, 0.0}}) {
        Brain field; useMatchKickParameters(field);
        field.config->fieldDimensions.length = 22;
        field.config->fieldDimensions.width = 14;
        field.data->robotPoseToField = {pose.x, pose.y, 0.0};
        field.data->tmMyCost = 20;
        localBall(field, 1.3, 0.0, 0.0);
        StrikerDecide decide("decide", {}, &field);
        check(runDecision(field, decide, "chase") == "auto_visual_kick",
              "cost or field-region heuristic blocks an owned kickable ball");
        // Configured larger power-kick box must not displace visual kick.
        field.parameters["strategy.power_shoot.enable"] = 1;
        field.parameters["strategy.power_shoot.xmax"] = 1.5;
        field.parameters["strategy.power_shoot.ymin"] = -0.3;
        field.parameters["strategy.power_shoot.ymax"] = 0.3;
        check(runDecision(field, decide, "chase") == "auto_visual_kick",
              "power-kick possibility prevents immediate SDK handoff");
        field.data->tmImLead = false;
        check(runDecision(field, decide, "chase") == "assist", "fast kick bypasses ownership");
        field.data->tmImLead = true; field.data->ballFromTeammate = true;
        check(runDecision(field, decide, "chase") == "chase", "fast kick trusts a shared-only ball");
    }

    Brain guards; useMatchKickParameters(guards); guards.angleGood = false;
    StrikerDecide guard("decide", {}, &guards);
    localBall(guards, 1.6, 0.0, 0.0);
    check(runDecision(guards, guard, "chase") == "chase", "far ball bypasses approach");
    localBall(guards, 0.6, 0.0, 1.3);
    check(runDecision(guards, guard, "adjust") == "reposition", "wrong-side ball bypasses orbit");
    localBall(guards, 0.6, 1.0, 0.0);
    check(runDecision(guards, guard, "adjust") != "auto_visual_kick", "side ball bypasses turning");
    localBall(guards, 0.7, 0.5, 0.2);
    guards.blocked = true;
    check(runDecision(guards, guard, "adjust") == "kick_wait", "fast kick bypasses teammate clearance");
    guards.blocked = false;
    for (const auto source : {kick_strategy::BallSource::none, kick_strategy::BallSource::memory,
                              kick_strategy::BallSource::predicted}) {
        guards.source = source;
        check(runDecision(guards, guard, "adjust") != "auto_visual_kick", "stale/predicted ball starts fast kick");
        RLVisionKick action("visual", {}, &guards);
        check(action.onStart() == NodeStatus::SUCCESS && guards.client->publisher->sent.empty(),
              "action starts a kick with no recent real ball");
    }

    Brain rejected; useMatchKickParameters(rejected);
    RLVisionKick retryAction("visual", {}, &rejected); request(rejected, retryAction);
    respond(rejected, sentStart(rejected), "{\"status\":9}"); retryAction.onRunning();
    advanceClock(rejected, 140);
    check(retryAction.onRunning() == NodeStatus::SUCCESS && cancelled(rejected), "rejection does not exit");
    StrikerDecide retry("decide", {}, &rejected);
    advanceClock(rejected, 159);
    check(runDecision(rejected, retry, "adjust") == "kick_wait", "failed start skips retry interval");
    advanceClock(rejected, 1);
    check(runDecision(rejected, retry, "adjust") == "auto_visual_kick", "retry still waits the old 1000 ms");

    Brain goalie; useMatchKickParameters(goalie);
    goalie.tree->setEntry("goalie_mode", string("block_kick"));
    RLVisionKick clearance("visual", {}, &goalie); clearance.onStart();
    advanceClock(goalie, 40); clearance.onRunning();
    check(goalie.client->visualKickStartState() == kick_strategy::StartState::pending,
          "goalie still waits the old 80 ms startup delay");
    clearance.onHalted();
    cout << "Fast match kick: first-tick priority, 40 ms request, 200 ms cooldown, 300 ms retry, guards passed\n";
}

void approachHandoffFlow() {
    Brain p3;
    localBall(p3, 0.352, 0.298, 2.53);
    StrikerDecide decide("decide", {}, &p3);
    Chase chase("chase", {}, &p3);
    chase.numbers["dist"] = 0.4;
    chase.numbers["safe_dist"] = 0.55;
    check(runDecision(p3, decide, "chase") == "reposition",
          "close ball on the wrong side is handed to fine adjustment");
    runChase(p3, chase, "reposition");
    const auto retreat = p3.client->command;
    check(retreat.x * cos(p3.data->ball.yawToRobot) +
              retreat.y * sin(p3.data->ball.yawToRobot) < 0.0,
          "close wrong-side ball does not first retreat to leave orbit clearance");
    check(hypot(retreat.x, retreat.y) <= 0.45 + 1e-9 && abs(retreat.yaw) <= 1.5,
          "coarse positioning exceeds configured speed caps");
    localBall(p3, 0.55, 0.0, 2.53);
    runChase(p3, chase, "reposition");
    check(p3.client->command.y < 0.0 && abs(p3.client->command.x) < 1e-9,
          "wrong-side position follows a direct path through the ball instead of orbiting");

    Brain hysteresis;
    StrikerDecide transition("decide", {}, &hysteresis);
    localBall(hysteresis, 0.7, 0.3, 0.64);
    check(runDecision(hysteresis, transition, "adjust") == "adjust",
          "coarse orbit starts below its entry threshold");
    localBall(hysteresis, 0.7, 0.3, 0.66);
    check(runDecision(hysteresis, transition, "adjust") == "reposition",
          "coarse orbit does not start above its entry threshold");
    localBall(hysteresis, 0.7, 0.3, 0.5);
    check(runDecision(hysteresis, transition, "reposition") == "reposition",
          "coarse orbit exits too early inside the hysteresis band");
    check(runDecision(hysteresis, transition, "adjust") == "adjust",
          "fine adjustment restarts orbit inside the hysteresis band");
    localBall(hysteresis, 0.7, 0.3, 0.36);
    check(runDecision(hysteresis, transition, "reposition") == "reposition",
          "coarse orbit exits above its exit threshold");
    localBall(hysteresis, 0.7, 0.3, 0.34);
    check(runDecision(hysteresis, transition, "reposition") == "adjust",
          "coarse orbit cannot hand off to fine adjustment");
    localBall(hysteresis, 1.2, 0.3, 2.5);
    check(runDecision(hysteresis, transition, "reposition") == "chase",
          "far ball is assigned near-ball orbit instead of approach");

    hysteresis.parameters["strategy.kick_approach.reposition_enter_angle"] = 0.9;
    hysteresis.parameters["strategy.kick_approach.reposition_exit_angle"] = 0.5;
    localBall(hysteresis, 0.7, 0.3, 0.8);
    check(runDecision(hysteresis, transition, "adjust") == "adjust" &&
          runDecision(hysteresis, transition, "reposition") == "reposition",
          "configured coarse orbit entry/exit thresholds are ignored");
    localBall(hysteresis, 0.7, 0.3, 0.49);
    check(runDecision(hysteresis, transition, "reposition") == "adjust",
          "configured orbit exit threshold is ignored");

    Brain rightSide;
    localBall(rightSide, 0.4, -0.55, 0.0);
    StrikerDecide facing("decide", {}, &rightSide);
    Adjust fine("adjust", {}, &rightSide);
    fine.numbers["range"] = 0.4;
    check(runDecision(rightSide, facing, "chase") == "adjust",
          "a correct-side ball with only body error starts another orbit");
    fine.tick();
    check(rightSide.client->command.x == 0.0 && rightSide.client->command.y == 0.0 &&
          rightSide.client->command.yaw < 0.0,
          "body-only adjustment walks away from an already aligned approach line");
    // Outside the shot-line tolerance, face the ball while orbiting. The old
    // 0.5-rad cutoff turned toward the shot too early and could lose the ball.
    localBall(rightSide, 0.4, -0.1, 0.3);
    fine.tick();
    check(rightSide.client->command.yaw < 0.0,
          "fine orbit rotates away from the ball before the route is aligned");

    Brain ready;
    StrikerDecide direct("decide", {}, &ready);
    localBall(ready, 0.4, 0.0, 0.0);
    check(runDecision(ready, direct, "reposition") == "auto_visual_kick",
          "an aligned ball must finish an unnecessary orbit before SDK kick");
    ready.parameters["strategy.enable_auto_visual_kick"] = 0.0;
    check(runDecision(ready, direct, "reposition") == "kick",
          "aligned ordinary kick is blocked by the new orbit stage");
    localBall(ready, 0.4, 0.0, 2.0);
    check(runDecision(ready, direct, "adjust") == "kick",
          "existing ordinary kick eligibility is narrowed by coarse positioning");
    ready.data->tmImLead = false;
    check(runDecision(ready, direct, "reposition") == "assist",
          "coarse positioning takes priority over revoked ownership");

    Brain bounded;
    bounded.parameters["strategy.kick_approach.orbit_speed"] = 0.8;
    bounded.parameters["strategy.kick_approach.orbit_distance"] = 0.7;
    bounded.config->limitNearBallSpeed = true;
    bounded.config->nearBallSpeedLimit = 0.2;
    localBall(bounded, 0.55, 0.0, 2.0);
    Chase limits("chase", {}, &bounded);
    limits.numbers["vx_limit"] = 0.6;
    limits.numbers["vy_limit"] = 0.4;
    limits.numbers["vtheta_limit"] = 0.2;
    runChase(bounded, limits, "reposition");
    check(bounded.client->command.x < 0.0 &&
          hypot(bounded.client->command.x, bounded.client->command.y) <= 0.2 + 1e-9,
          "configured stand-off or near-ball speed limit is ignored during orbit");
    localBall(bounded, 0.7, 0.8, 2.0);
    runChase(bounded, limits, "reposition");
    check(bounded.client->command.x == 0.0 && bounded.client->command.y == 0.0 &&
          bounded.client->command.yaw > 0.0 && bounded.client->command.yaw <= 0.2,
          "sideways ball is translated toward before a bounded turn");

    // Integrate a stationary ball and ideal walking velocities, exercising
    // decision handoff and production Chase together rather than a copy of
    // their equations. This detects wrong orbit sign and ball-crossing routes.
    Brain orbit;
    orbit.angleGood = false;
    localBall(orbit, 0.352, 0.298, 2.53);
    const auto fixedBall = orbit.data->ball.posToField;
    const double shotDirection = orbit.data->kickDir;
    StrikerDecide planner("decide", {}, &orbit);
    Chase motion("chase", {}, &orbit);
    string previous = "chase";
    bool reachedFine = false;
    for (int step = 0; step < 400; ++step) {
        const string decision = runDecision(orbit, planner, previous);
        if (decision != "reposition") {
            check(decision == "adjust" || decision == "auto_visual_kick",
                  "coarse orbit unexpectedly abandons its owned local ball");
            check(abs(toPInPI(shotDirection - orbit.data->robotBallAngleToField)) <= 0.35,
                  "coarse orbit hands off while the route is still on the wrong side");
            reachedFine = true;
            break;
        }
        runChase(orbit, motion, decision);
        const auto command = orbit.client->command;
        auto &pose = orbit.data->robotPoseToField;
        const double dt = 0.05;
        pose.x += (command.x * cos(pose.theta) - command.y * sin(pose.theta)) * dt;
        pose.y += (command.x * sin(pose.theta) + command.y * cos(pose.theta)) * dt;
        pose.theta = toPInPI(pose.theta + command.yaw * dt);
        advanceClock(orbit, dt * 1000.0);
        const double range = hypot(fixedBall.x - pose.x, fixedBall.y - pose.y);
        const double route = atan2(fixedBall.y - pose.y, fixedBall.x - pose.x);
        localBall(orbit, range, toPInPI(route - pose.theta), toPInPI(shotDirection - route));
        check(range >= 0.3, "coarse positioning trajectory crosses through the ball");
        previous = decision;
    }
    check(reachedFine, "coarse orbit cannot converge to the fine-adjustment handoff");
}

void approachBoundaryFlow() {
    Brain wrap;
    Chase route("chase", {}, &wrap);
    localBall(wrap, 0.55, 0.0, kick_geometry::kPi - 0.02);
    runChase(wrap, route, "reposition");
    check(wrap.client->command.y < 0.0, "initial orbit direction is reversed");
    advanceClock(wrap, 30);
    localBall(wrap, 0.55, 0.0, -kick_geometry::kPi + 0.02);
    runChase(wrap, route, "reposition");
    check(wrap.client->command.y < 0.0, "angle wrap reverses a locked production orbit");
    localBall(wrap, 0.55, 0.0, -1.0);
    runChase(wrap, route, "reposition");
    check(wrap.client->command.y > 0.0, "changed shot direction keeps an obsolete orbit plan");
    wrap.data->robotPoseToField.x += 0.5;
    localBall(wrap, 0.55, 0.0, 1.0);
    runChase(wrap, route, "reposition");
    check(wrap.client->command.y < 0.0, "moved ball keeps an obsolete orbit direction");
    advanceClock(wrap, 600);
    localBall(wrap, 0.55, 0.0, -1.0);
    runChase(wrap, route, "reposition");
    check(wrap.client->command.y > 0.0, "resumed orbit reuses a stale inactive plan");
    wrap.data->tmImLead = false;
    runChase(wrap, route, "reposition");
    check(bodyStopped(wrap), "revoked owner continues coarse orbit motion");
    wrap.data->tmImLead = true;
    wrap.source = kick_strategy::BallSource::none;
    runChase(wrap, route, "reposition");
    check(bodyStopped(wrap), "expired local vision continues coarse orbit motion");

    Brain obstacle;
    obstacle.parameters["obstacle_avoidance.avoid_during_chase"] = 1.0;
    obstacle.parameters["obstacle_avoidance.chase_ao_safe_dist"] = 1.5;
    obstacle.blockedOrbitSide = -1;
    localBall(obstacle, 0.55, 0.0, 1.5);
    Chase avoiding("chase", {}, &obstacle);
    runChase(obstacle, avoiding, "reposition");
    check(bodyStopped(obstacle), "blocked orbit moves through the obstacle");
    advanceClock(obstacle, 310);
    runChase(obstacle, avoiding, "reposition");
    check(obstacle.client->command.y > 0.0,
          "persistent blocked orbit does not try the clear opposite side");
    obstacle.blockedOrbitSide = 1;
    runChase(obstacle, avoiding, "reposition");
    for (int i = 0; i < 6; ++i) {
        advanceClock(obstacle, 200);
        runChase(obstacle, avoiding, "reposition");
        check(bodyStopped(obstacle), "same target repeatedly flips orbit side under obstruction");
    }

    Brain both;
    both.parameters["obstacle_avoidance.avoid_during_chase"] = 1.0;
    both.parameters["obstacle_avoidance.chase_ao_safe_dist"] = 1.5;
    both.blockAllDirections = true;
    localBall(both, 0.55, 0.0, 1.5);
    Chase trapped("chase", {}, &both);
    runChase(both, trapped, "reposition");
    advanceClock(both, 310);
    runChase(both, trapped, "reposition");
    check(bodyStopped(both), "two blocked orbit directions still generate translation");
}

void sharedRecoveryFlow() {
    Brain owner;
    Chase chase("chase", {}, &owner);
    StrikerDecide decide("decide", {}, &owner);
    CamTrackBall head("head", {}, &owner);
    sharedReport(owner, 2.0, 1.2, true);
    decide.tick(); chase.tick(); head.tick();
    check(decide.strings["decision_out"] == "chase", "shared ball owner is allowed to kick before real detection");
    check(owner.data->sharedBallRecovery.phase() == team_ball::RecoveryPhase::turn &&
          owner.client->command.x == 0.0 && owner.client->command.y == 0.0 &&
          owner.client->command.yaw > 0.0 && owner.client->command.yaw <= 0.8,
          "shared target at large bearing does not turn in place within yaw limit");
    check(owner.client->heads.back().second == 1.1, "shared target head yaw exceeds hardware range");

    sharedReport(owner, 2.0, -1.2, true); chase.tick();
    check(owner.client->command.x == 0.0 && owner.client->command.yaw < 0.0 &&
          owner.client->command.yaw >= -0.8, "right-hand shared target turns in the wrong direction");
    chase.numbers["vtheta_limit"] = 0.3; chase.tick();
    check(abs(owner.client->command.yaw) <= 0.3, "shared turning ignores behavior tree yaw limit");
    chase.numbers["vtheta_limit"] = 1.5;

    sharedReport(owner, 2.0, 0.1, true); chase.tick();
    check(owner.data->sharedBallRecovery.phase() == team_ball::RecoveryPhase::approach &&
          owner.client->command.x > 0.0 && owner.client->command.x <= 0.6 &&
          owner.client->command.y == 0.0, "aligned shared target cannot be approached");
    sharedReport(owner, 0.5, 0.1, true); chase.tick(); head.tick();
    check(owner.data->sharedBallRecovery.phase() == team_ball::RecoveryPhase::scan && bodyStopped(owner),
          "near shared target does not stop both walking and turning for reacquisition");
    const auto initialHead = owner.client->heads.back();
    advanceClock(owner, 650); sharedReport(owner, 0.5, 0.1, true); head.tick();
    check(owner.client->heads.back() != initialHead, "shared target head remains fixed after scan delay");
    check(owner.data->tmImLead, "receiving the same target prematurely releases the owner");
    const int motionCount = owner.client->moves;
    double minPitch = initialHead.first, maxPitch = initialHead.first;
    double minYaw = initialHead.second, maxYaw = owner.client->heads.back().second;
    for (int step = 0; step < 6; ++step) {
        advanceClock(owner, 250); sharedReport(owner, 0.5, 0.1, true); head.tick();
        const auto position = owner.client->heads.back();
        check(position.first >= 0.0 && position.first <= 1.1 &&
              position.second >= -1.1 && position.second <= 1.1,
              "shared scan exceeds head motion bounds");
        minPitch = min(minPitch, position.first); maxPitch = max(maxPitch, position.first);
        minYaw = min(minYaw, position.second); maxYaw = max(maxYaw, position.second);
    }
    check(minYaw < initialHead.second && maxYaw > initialHead.second &&
          minPitch < initialHead.first && maxPitch > initialHead.first,
          "shared reacquisition does not scan both yaw and pitch around target");
    check(owner.client->moves == motionCount, "head scanning overwrites body motion commands");
    advanceClock(owner, 400); sharedReport(owner, 0.5, 0.1, true); chase.tick(); decide.tick();
    check(!owner.data->tmImLead && owner.data->sharedBallRecovery.phase() == team_ball::RecoveryPhase::released &&
          bodyStopped(owner) && decide.strings["decision_out"] == "assist",
          "repeated shared reports reset timeout or retain pursuit after failed reacquisition");

    Brain support;
    sharedReport(support, 2.0, 0.1, false);
    StrikerDecide assist("decide", {}, &support); Chase nonOwner("chase", {}, &support);
    CamTrackBall supportHead("head", {}, &support);
    assist.tick(); support.client->setVelocity(0.5, 0, 0.4); nonOwner.tick(); supportHead.tick();
    check(assist.strings["decision_out"] == "assist" && bodyStopped(support) &&
          support.data->sharedBallRecovery.phase() == team_ball::RecoveryPhase::support &&
          !support.client->heads.empty(), "support striker pursues shared ball or cannot look toward it");

    Brain tuned;
    tuned.data->sharedBallRecoverySettings.stopDistance = 0.9;
    tuned.data->sharedBallRecoverySettings.turnAngle = 0.4;
    tuned.data->sharedBallRecoverySettings.yawLimit = 0.25;
    Chase configured("chase", {}, &tuned);
    sharedReport(tuned, 1.2, 0.5, true); configured.tick();
    check(tuned.client->command.x == 0.0 && tuned.client->command.yaw > 0.0 &&
          tuned.client->command.yaw <= 0.25, "configured shared turn threshold/yaw cap is ignored");
    sharedReport(tuned, 0.8, 0.1, true); configured.tick();
    check(tuned.data->sharedBallRecovery.phase() == team_ball::RecoveryPhase::scan && bodyStopped(tuned),
          "configured shared stand-off distance is ignored");

    Brain stale;
    sharedReport(stale, 2.0, 0.1, true);
    Chase expired("chase", {}, &stale); CamTrackBall staleHead("head", {}, &stale);
    expired.tick(); staleHead.tick();
    const auto headCount = stale.client->heads.size();
    // A stale position may remain stored for diagnostics; reliability is decisive.
    stale.tree->setEntry("tm_ball_pos_reliable", false);
    expired.tick(); staleHead.tick();
    check(bodyStopped(stale) && stale.client->heads.size() == headCount,
          "expired shared position keeps driving body or head with ball_location_known still set");
    stale.tree->setEntry("ball_location_known", false);
    StrikerDecide lost("decide", {}, &stale); lost.tick();
    check(lost.strings["decision_out"] == "find", "expired shared position cannot resume ordinary search");

    Brain limits;
    sharedReport(limits, 0.5, 0.0, true);
    limits.data->tmBall.pitchToRobot = 1.5; limits.data->tmBall.yawToRobot = 2.0;
    limits.data->ball.pitchToRobot = 1.5; limits.data->ball.yawToRobot = 2.0;
    CamTrackBall limitedHead("head", {}, &limits); limitedHead.tick();
    check(limits.client->heads.back() == pair<double, double>{1.1, 1.1}, "shared head upper limits are not enforced");
    limits.data->tmBall.pitchToRobot = -0.3; limits.data->tmBall.yawToRobot = -2.0;
    limits.data->ball.pitchToRobot = -0.3; limits.data->ball.yawToRobot = -2.0;
    limitedHead.tick();
    check(limits.client->heads.back() == pair<double, double>{0.0, -1.1}, "shared head lower limits are not enforced");

    // Restore fresh local detection after release. Recovery resets and pixels,
    // rather than stale shared geometry or scan offsets, drive the head again.
    owner.data->ballFromTeammate = false; owner.data->ballDetected = true;
    owner.source = kick_strategy::BallSource::real; owner.data->tmImLead = true;
    owner.data->ball.range = 0.4; owner.data->ball.posToRobot = {0.4, 0.0, 0.0};
    owner.data->ball.yawToRobot = 0.0;
    owner.data->ball.boundingBox.xmin = 560; owner.data->ball.boundingBox.xmax = 600;
    owner.data->ball.boundingBox.ymin = 410; owner.data->ball.boundingBox.ymax = 450;
    owner.data->sharedBallRecovery.canPursue(monotonicMs, false, true, 0.4, 1.0, 0.0);
    head.tick(); decide.tick();
    const auto localHead = owner.client->heads.back();
    check(owner.data->sharedBallRecovery.phase() == team_ball::RecoveryPhase::inactive &&
          localHead.second < owner.data->headYaw && localHead.first > owner.data->headPitch &&
          decide.strings["decision_out"] == "auto_visual_kick",
          "fresh local detection fails to end shared search and resume real-ball tracking/kicking");
}

int main() {
    try {
        fastKickFlow();
        sharedRecoveryFlow();
        approachBoundaryFlow();
        approachHandoffFlow();
        Brain known; MoveToFarSetPlaySearchArea unnecessary("search", {}, &known);
        known.data->robotPoseToField.x = -1;
        check(unnecessary.onStart() == NodeStatus::SUCCESS, "known local ball is preempted by half-field search");
        known.tree->setEntry("ball_location_known", false); known.tree->setEntry("tm_ball_pos_reliable", true);
        check(unnecessary.onStart() == NodeStatus::SUCCESS, "reliable shared ball is preempted by search");
        Brain found; found.tree->setEntry("ball_location_known", false); found.data->robotPoseToField.x = -1;
        MoveToFarSetPlaySearchArea searching("search", {}, &found);
        check(searching.onStart() == NodeStatus::RUNNING, "unknown far ball did not start a search");
        found.tree->setEntry("ball_location_known", true);
        check(searching.onRunning() == NodeStatus::SUCCESS, "finding ball on original half did not release search");
        Brain timed; timed.tree->setEntry("ball_location_known", false); timed.data->robotPoseToField.x = -1;
        MoveToFarSetPlaySearchArea bounded("search", {}, &timed); bounded.onStart(); advanceClock(timed, 30001);
        check(bounded.onRunning() == NodeStatus::SUCCESS, "timed-out search blocks striker forever");
        check(bounded.onStart() == NodeStatus::SUCCESS, "timed-out search immediately restarts in same set play");
        ++timed.data->setPlaySearchEpoch;
        check(bounded.onStart() == NodeStatus::RUNNING, "new set play cannot start a new search");
        Brain unlocated; unlocated.tree->setEntry("ball_location_known", false);
        unlocated.tree->setEntry("odom_calibrated", false);
        MoveToFarSetPlaySearchArea cannotMove("search", {}, &unlocated);
        check(cannotMove.onStart() == NodeStatus::SUCCESS, "missing localization indefinitely preempts local finding");

        Brain tracking; tracking.data->tmImInVisualKick = true; tracking.data->ballDetected = false;
        tracking.source = kick_strategy::BallSource::recentReal; tracking.data->ball.yawToRobot = 0.18;
        CamTrackBall head("head", {}, &tracking); head.tick();
        check(tracking.client->heads.size() == 1 && tracking.client->heads.back().first == 0.9 &&
              tracking.client->heads.back().second == 0.18, "visual session loses near-ball head tracking");
        advanceClock(tracking, 10); head.tick();
        check(tracking.client->heads.size() == 1, "visual tracking floods head commands");
        advanceClock(tracking, 100); head.tick();
        check(tracking.client->heads.size() == 2, "visual head tracking never refreshes");

        Brain blocked; blocked.blocked = true; StrikerDecide decide("decide", {}, &blocked); decide.tick();
        check(decide.strings["decision_out"] == "kick_wait", "blocked striker keeps selecting SDK start");
        RLVisionKick deferred("visual", {}, &blocked); blocked.client->setVelocity(0.5, 0, 0);
        check(deferred.onStart() == NodeStatus::SUCCESS && blocked.client->command.x == 0,
              "deferred start leaves old velocity active");
        blocked.blocked = false; decide.tick();
        check(decide.strings["decision_out"] == "auto_visual_kick", "clear corridor cannot start kick");
        blocked.data->tmImLead = false; decide.tick();
        check(decide.strings["decision_out"] == "assist", "striker ignores revoked ownership");

        Brain dribble; dribble.data->robotPoseToField.theta = 0.3; dribble.data->ballDetected = false;
        dribble.source = kick_strategy::BallSource::recentReal;
        StrikerDecide continueKick("decide", {}, &dribble); continueKick.tick();
        check(continueKick.strings["decision_out"] == "kick", "recent real ball cannot enter ordinary kick");
        dribble.data->tmImInDribbleKick = true; continueKick.strings["decision_in"] = "power_kick";
        continueKick.tick();
        check(continueKick.strings["decision_out"] == "power_kick", "missed frame switches a running dribble");
        dribble.source = kick_strategy::BallSource::none; continueKick.tick();
        check(continueKick.strings["decision_out"] == "adjust", "expired real source keeps dribble decision alive");

        Brain accepted; RLVisionKick visual("visual", {}, &accepted); request(accepted, visual);
        const string uuid = sentStart(accepted);
        respond(accepted, "wrong-uuid", "{\"status\":0}");
        check(accepted.client->visualKickStartState() == kick_strategy::StartState::pending, "unrelated response grants SDK start");
        respond(accepted, uuid, "{\"status\":0}"); visual.onRunning();
        check(accepted.client->visualKickStartState() == kick_strategy::StartState::accepted, "matching success was not accepted");
        respond(accepted, uuid, "{\"status\":9}");
        check(accepted.client->visualKickStartState() == kick_strategy::StartState::accepted,
              "duplicate response changes an accepted request");
        check(accepted.client->heads.empty(), "SDK node forced a head scan instead of leaving tracking active");
        accepted.source = kick_strategy::BallSource::none; visual.onRunning(); advanceClock(accepted, 141);
        check(visual.onRunning() == NodeStatus::SUCCESS && cancelled(accepted), "expired ball source does not exit SDK");

        for (const string header : {string("{\"status\":9}"), string("{}"), string("malformed"),
                                   string("{\"status\":4294967296}"), string("{\"status\":0.0}")}) {
            Brain rejected; RLVisionKick action("visual", {}, &rejected); request(rejected, action);
            respond(rejected, sentStart(rejected), header); action.onRunning(); advanceClock(rejected, 141);
            check(action.onRunning() == NodeStatus::SUCCESS && cancelled(rejected), "rejected or malformed start remains active");
            StrikerDecide retry("decide", {}, &rejected); retry.tick();
            check(retry.strings["decision_out"] == "kick_wait", "failed start has no retry backoff");
            advanceClock(rejected, 1001); retry.tick();
            check(retry.strings["decision_out"] == "auto_visual_kick", "retry never recovers after backoff");
        }
        Brain timeout; RLVisionKick noAck("visual", {}, &timeout); request(timeout, noAck);
        const string expiredUuid = sentStart(timeout); advanceClock(timeout, 1201);
        respond(timeout, expiredUuid, "{\"status\":0}");
        check(timeout.client->visualKickStartState() == kick_strategy::StartState::timedOut,
              "late response bypasses timeout before the next node tick");
        noAck.onRunning(); advanceClock(timeout, 141);
        check(noAck.onRunning() == NodeStatus::SUCCESS && cancelled(timeout), "missing SDK response never times out/cancels");
        respond(timeout, expiredUuid, "{\"status\":0}");
        check(timeout.client->visualKickStartState() == kick_strategy::StartState::idle, "late acknowledgement revives cancelled start");
        Brain halted; RLVisionKick pending("visual", {}, &halted); request(halted, pending); pending.onHalted();
        check(cancelled(halted) && !halted.data->tmImInVisualKick, "tree halt does not cancel unacknowledged SDK request");

        for (int cause = 0; cause < 3; ++cause) {
            Brain interrupted; RLVisionKick action("visual", {}, &interrupted); request(interrupted, action);
            if (cause == 0) interrupted.data->tmImLead = false;
            if (cause == 1) interrupted.blocked = true;
            if (cause == 2) interrupted.data->shouldExitRLVisionKick = true;
            auto result = action.onRunning();
            if (result == NodeStatus::RUNNING) { advanceClock(interrupted, 141); result = action.onRunning(); }
            check(result == NodeStatus::SUCCESS && cancelled(interrupted) && !interrupted.data->tmImInVisualKick,
                  "ownership/corridor/recovery does not cancel pending SDK start");
        }
        Brain replaced; replaced.client->RLVisionKick(true); const string previous = sentStart(replaced);
        replaced.client->RLVisionKick(true); const string current = sentStart(replaced);
        respond(replaced, previous, "{\"status\":0}");
        check(replaced.client->visualKickStartState() == kick_strategy::StartState::pending,
              "old request response confirms a replacement");
        respond(replaced, current, "{\"status\":0}");
        check(replaced.client->visualKickStartState() == kick_strategy::StartState::accepted,
              "replacement cannot accept its own response");
        replaced.client->robocupWalk();
        cout << "Soccer flow: shared recovery, close-ball retreat/orbit/handoff, hysteresis, speed limits, direct kick priority, ownership, search release, missed frames, RPC lifecycle passed\n";
    } catch (const exception &error) { cerr << error.what() << '\n'; return 1; }
}
