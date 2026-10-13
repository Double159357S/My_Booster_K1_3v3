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
    map<string, double> numbers{{"chase_threshold", 1.0}, {"min_msec_kick", 1500}, {"max_msec_kick", 7000}, {"range", 6.0}};
    map<string, string> strings{{"decision_in", ""}, {"position", "offense"}};
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
    string kickType = "shoot", realGameSubState = "CORNER_KICK", goalieVisualKickSubState = "none";
    Point goalieVisualKickTargetField;
    vector<FakePeer> tmStatus = vector<FakePeer>(5);
    uint64_t setPlaySearchEpoch = 1;
};
struct FakeConfig {
    int numOfPlayers = 3, playerId = 1;
    bool enableCom = true;
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
    bool blocked = false;
    kick_strategy::BallSource source = kick_strategy::BallSource::real;
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
    bool isAngleGood(double, const string &) const { return true; }
    bool isAngleGoodForDirectionalKick(double) const { return true; }
    double distToObstacle(double) const { return 100.0; }
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

int main() {
    try {
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
        cout << "Soccer flow: search release, wait/resume, missed frames, RPC acceptance/rejection/timeout/cancellation passed\n";
    } catch (const exception &error) { cerr << error.what() << '\n'; return 1; }
}
