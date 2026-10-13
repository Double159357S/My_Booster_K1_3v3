// Boundary fakes for executing the actual Kick and crabWalk function bodies
// without ROS, the Booster SDK, or a robot connection.
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "utils/kick_strategy.h"
using namespace std;

namespace rclcpp {
struct Duration { double ms; Duration(int sec, int ns) : ms(sec * 1000.0 + ns / 1e6) {} };
struct Time { double ms = 0.0; Time &operator-=(Duration d) { ms -= d.ms; return *this; } };
}
namespace rerun { struct TextLog { explicit TextLog(string) {} }; }
enum class NodeStatus { SUCCESS, RUNNING };
string format(const char *message, ...) { return message; }
void prtErr(const string &) {}
double cap(double value, double high, double low) { return clamp(value, low, high); }

struct FakeClock { rclcpp::Time time; rclcpp::Time now() const { return time; } };
struct FakeTree {
    int control = 3;
    bool assist = false, ballOut = false;
    template<typename T> T getEntry(const string &key) const {
        if constexpr (is_same_v<T, string>) return "striker";
        if constexpr (is_same_v<T, int>) return control;
        if constexpr (is_same_v<T, bool>) return key == "assist_kick" ? assist : ballOut;
        return {};
    }
};
struct FakeLog { void setTimeNow() {} void log(const string &, rerun::TextLog) {} };
struct FakeBall {
    struct { double x = 0.4, y = 0.02; } posToRobot;
    double range = 0.4, yawToRobot = 0.05;
    rclcpp::Time timePoint;
};
struct FakeData {
    FakeBall ball;
    struct { double x = 0.0; } robotPoseToField;
    bool tmImLead = true, ballDetected = true;
    bool isFreekickKickingOff = false, isKickingOff = false;
};
struct FakeConfig {
    bool enableStableKick = false;
    double vxFactor = 0.9, yawOffset = 0.0, vxLimit = 1.6, vyLimit = 0.8;
    struct { double length = 9.0, goalAreaLength = 1.0; } fieldDimensions;
};
class Brain;
class RobotClient {
public:
    Brain *brain;
    struct Command { double x, y, yaw; } command{};
    vector<Command> history;
    int setVelocity(double x, double y, double yaw, bool minX = true, bool minY = true, bool minYaw = true) {
        const auto minimum = [](double v) {
            return abs(v) > 1e-5 && abs(v) < 0.3 ? copysign(0.3, v) : v;
        };
        command = {minX ? minimum(x) : x, minY ? minimum(y) : y, minYaw ? minimum(yaw) : yaw};
        history.push_back(command);
        return 0;
    }
    int crabWalk(double angle, double speed);
};
class Brain {
public:
    FakeTree treeStorage; FakeTree *tree = &treeStorage;
    FakeData dataStorage; FakeData *data = &dataStorage;
    FakeConfig configStorage; FakeConfig *config = &configStorage;
    FakeLog logStorage; FakeLog *log = &logStorage;
    FakeClock clock;
    RobotClient clientStorage{this}; RobotClient *client = &clientStorage;
    kick_strategy::BallSource source = kick_strategy::BallSource::real;
    bool avoid = false, soft = true, abortMoved = true;
    double obstacle = 100.0;
    FakeClock *get_clock() { return &clock; }
    double msecsSince(rclcpp::Time time) const { return clock.time.ms - time.ms; }
    auto getKickBallSource() const { return source; }
    void get_parameter(const string &name, bool &out) const {
        out = name == "strategy.soft_kickoff" ? soft :
              name == "strategy.abort_kick_when_ball_moved" ? abortMoved : avoid;
    }
    void get_parameter(const string &name, double &out) const {
        out = name == "strategy.kick_range" ? 1.0 :
              name == "obstacle_avoidance.kick_ao_safe_dist" ? 1.5 : 0.3;
    }
    double threatLevel() const { return 0.0; }
    double distToObstacle(double) const { return obstacle; }
};
class Kick {
public:
    Brain *brain;
    map<string, double> inputs{{"speed_limit", 1.4}, {"min_msec_kick", 1000}, {"msecs_stablize", 300}};
    explicit Kick(Brain *b) : brain(b) {}
    template<typename T> optional<T> getInput(const string &name) const { return static_cast<T>(inputs.at(name)); }
    NodeStatus onStart(); NodeStatus onRunning(); void onHalted();
private:
    rclcpp::Time _startTime, _lastSpeedUpdate;
    string _state = "kick";
    int _msecKick = 1000;
    double _speed = 0.0, _minRange = 0.0, _speedLimit = 0.0;
};

// PRODUCTION_FUNCTIONS

void check(bool value, const char *message) { if (!value) throw runtime_error(message); }
bool stopped(const Brain &b) {
    const auto &c = b.client->command;
    return abs(c.x) < 1e-9 && abs(c.y) < 1e-9 && abs(c.yaw) < 1e-9;
}
void tick(Brain &b, double ms) { b.clock.time.ms = ms; b.data->ball.timePoint.ms = ms; }

int main() {
    try {
        Brain normal; Kick kick(&normal);
        check(kick.onStart() == NodeStatus::RUNNING, "valid dribble did not start");
        // A small angle must stay small instead of boosting lateral speed.
        const auto &c = normal.client->command;
        check(abs(c.y / c.x - tan(0.05) / 0.9) < 1e-9, "crabWalk changed the kicking direction");
        tick(normal, 10); kick.onRunning();
        check(normal.client->command.x < 0.3, "dribble immediately jumps to full speed");
        normal.data->tmImLead = false;
        check(kick.onRunning() == NodeStatus::SUCCESS && stopped(normal), "revoked owner keeps walking");

        for (int cause = 0; cause < 5; ++cause) {
            Brain b; Kick action(&b); action.onStart(); tick(b, 10);
            if (cause == 0) b.source = kick_strategy::BallSource::none;
            if (cause == 1) b.data->ball.posToRobot.x = -0.1;
            if (cause == 2) b.data->ball.range = 1.2;
            if (cause == 3) b.data->ball.range = 0.8;
            if (cause == 4) { b.avoid = true; b.obstacle = 0.4; }
            check(action.onRunning() == NodeStatus::SUCCESS && stopped(b), "aborted kick left a motion command active");
        }
        Brain halt; Kick halted(&halt); halted.onStart(); halted.onHalted();
        check(stopped(halt), "tree halt left dribble velocity active");

        Brain kickoff; kickoff.data->isFreekickKickingOff = true; Kick soft(&kickoff);
        soft.onStart();
        for (int i = 1; i <= 100; ++i) {
            tick(kickoff, i * 10);
            check(soft.onRunning() == NodeStatus::RUNNING, "soft dribble exited too early");
            check(hypot(kickoff.client->command.x, kickoff.client->command.y) <= 0.3 + 1e-9,
                  "soft kickoff accelerates past its speed limit during execution");
        }
        Brain stable; stable.config->enableStableKick = true; Kick settling(&stable);
        settling.onStart(); check(stopped(stable), "stabilization backs away from kick stance");
        tick(stable, 301); settling.onRunning(); check(!stopped(stable), "stable kick never starts moving");

        Brain timeout; timeout.abortMoved = false; Kick bounded(&timeout); bounded.onStart();
        timeout.data->ball.range = 0.9; tick(timeout, 1400);
        check(bounded.onRunning() == NodeStatus::SUCCESS && stopped(timeout), "moving ball indefinitely extends the deadline");
        Brain denied; denied.data->tmImLead = false; Kick unowned(&denied);
        check(unowned.onStart() == NodeStatus::SUCCESS && stopped(denied), "unowned autonomous dribble started");
        denied.tree->assist = true;
        check(unowned.onStart() == NodeStatus::RUNNING, "explicit manual kick was blocked by team ownership");
        cout << "Kick runtime: direction, ramp, soft kickoff, ownership, abort, halt and deadline passed\n";
    } catch (const exception &e) { cerr << e.what() << '\n'; return 1; }
}
