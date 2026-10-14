#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

#include "team_communication_msg.h"
#include "utils/team_ball.h"
#include "utils/kick_strategy.h"
#include "utils/goalie_strategy.h"
#include "utils/ready_formation.h"

using namespace team_cooperation;

namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void near(double value, double expected, const char *message) {
    check(std::fabs(value - expected) < 1e-8, message);
}

// Tick at 100 Hz and publish at 10 Hz, matching the actual brain/UDP threads.
// Every sample checks both ownership and the 700 ms physical kick exit ramp.
struct Network {
    struct Node {
        Arbitration arbitration;
        bool alive = true, canControl = true, kicking = false, owns = false;
        bool nearVisibleBall = false;
        double cost = 4.0, exitUntil = 0.0;
        std::array<Peer, 3> received{};
        std::array<double, 3> receivedAt{{-10000, -10000, -10000}};
        Node() { for (auto &peer : received) peer.ageMs = -1.0; }
    };
    struct Packet { int from, to; double due; Peer peer; };
    std::array<Node, 3> nodes;
    bool links[3][3] = {{true, true, true}, {true, true, true}, {true, true, true}};
    std::vector<Packet> packets;
    Settings settings;
    double now = 0.0;
    int tick = 0;
    bool randomLoss = false;
    std::mt19937 random{7043};

    Network() { nodes[0].canControl = false; }
    void isolate(int id, bool isolated) {
        for (int i = 0; i < 3; ++i) links[id - 1][i] = links[i][id - 1] = !isolated;
    }
    void run(double duration) {
        const int steps = static_cast<int>(duration / 10.0);
        for (int step = 0; step < steps; ++step, now += 10.0, ++tick) {
            for (auto it = packets.begin(); it != packets.end();) {
                if (it->due > now) { ++it; continue; }
                if (links[it->from][it->to]) {
                    // Model the receiver's sequence filter: old packets cannot
                    // replace a newer broadcast from the same process.
                    auto &node = nodes[it->to];
                    if (it->peer.ageMs > node.received[it->from].ageMs) {
                        node.received[it->from] = it->peer;
                        node.receivedAt[it->from] = now;
                    }
                }
                it = packets.erase(it);
            }
            int owners = 0, physicalKickers = 0;
            for (int i = 0; i < 3; ++i) {
                auto &n = nodes[i];
                std::vector<Peer> peers{{i + 1, n.alive, n.canControl, n.kicking,
                    n.cost, 0.0, n.arbitration.announcement(now), n.nearVisibleBall}};
                for (int j = 0; j < 3; ++j) if (i != j) {
                    auto peer = n.received[j];
                    peer.ageMs = now - n.receivedAt[j];
                    peers.push_back(peer);
                }
                const bool wasOwner = n.owns;
                n.owns = n.arbitration.update(now, i + 1, 2, peers, settings);
                if (wasOwner && !n.owns && n.kicking) n.exitUntil = now + 700.0;
                owners += n.owns;
                physicalKickers += (n.owns || now < n.exitUntil);
            }
            check(owners <= 1, "multiple owners in network simulation");
            check(physicalKickers <= 1, "new owner starts before old kicker exits");
            if (tick % 10 == 0) {
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
                    if (i == j || !links[i][j]) continue;
                    if (randomLoss && random() % 5 == 0) continue;
                    const double delay = randomLoss ? 10.0 + random() % 120 : 20.0;
                    auto &n = nodes[i];
                    packets.push_back({i, j, now + delay,
                        {i + 1, n.alive, n.canControl, n.kicking || now < n.exitUntil,
                         n.cost, now + 1.0, n.arbitration.announcement(now), n.nearVisibleBall}});
                }
            }
        }
    }
    int owner() const {
        for (int i = 0; i < 3; ++i) if (nodes[i].owns) return i + 1;
        return 0;
    }
};

void arbitrationTests() {
    Network net;
    net.run(3000);
    check(net.owner() == 2, "equal costs should choose lower striker ID");
    for (int i = 0; i < 30; ++i) {
        net.nodes[1].cost = i % 2 == 0 ? 4.2 : 3.8;
        net.nodes[2].cost = 4.0;
        net.run(100);
        check(net.owner() == 2, "small cost jitter changed owner");
    }
    net.nodes[1].kicking = true;
    net.nodes[2].cost = 1.0;
    net.run(2500);
    check(net.owner() == 2, "cost improvement interrupted active kick");
    net.nodes[1].kicking = false;
    net.run(2200);
    check(net.owner() == 3, "sustained cost improvement did not hand off");
    net.nodes[2].kicking = true;
    net.nodes[2].canControl = false;
    net.run(2800);
    check(net.owner() == 2, "unready owner did not release ball");

    net.isolate(1, true);
    net.run(4000);
    check(net.owner() == 2, "coordinator loss did not recover with majority");
    net.nodes[0].arbitration = Arbitration{};
    net.isolate(1, false);
    net.run(4000);
    check(net.owner() == 2, "cold coordinator restart did not recover");
    net.isolate(2, true);
    net.isolate(3, true);
    net.run(1000);
    check(net.owner() == 0, "isolated robots must not elect themselves");

    for (int seed = 0; seed < 12; ++seed) {
        Network lossy;
        lossy.randomLoss = true;
        lossy.random.seed(seed);
        lossy.run(4000);
        check(lossy.owner() == 2, "packet loss prevented initial majority grant");
        lossy.nodes[2].cost = 1.0;
        lossy.run(3500);
        check(lossy.owner() == 3, "packet loss prevented stable handoff");
    }

    Arbitration follower;
    std::vector<Peer> peers{{1, true, false, false, 10, 0, {1, 2, 7, 300}},
                            {2, true, true, false, 2, 0, {}}};
    check(follower.update(1000, 2, 2, peers, Settings{}), "follower did not adopt grant");
    peers[0].ageMs = 350;
    check(!follower.update(1350, 2, 2, peers, Settings{}), "cached packet renewed expired grant");
    check(follower.announcement(1350).leaseMs == 0, "expired grant still advertised a lease");
    peers[0].decision = {1, 2, 7, 1201};
    check(!follower.update(1350, 2, 2, peers, Settings{}), "oversized lease was adopted");
}

template<class T>
void fillPacket(T &p) {
    p.communicationId = 12;
    p.teamId = 70;
    p.playerId = 2;
    p.playerRole = 1;
    p.isAlive = true;
    p.ballDetected = true;
    p.ballLocationKnown = true;
    p.ballConfidence = 90;
    p.ballRange = 0.5;
    p.cost = 4.0;
    p.ballPosToField = {1.0, 2.0, 0.0};
    p.robotPoseToField = {0.0, 1.0, 0.2};
    p.cmdId = 15;
}

void packetTests() {
    TeamOwnershipPacket packet{}, decoded{};
    fillPacket(packet.state);
    packet.state.validation = VALIDATION_COMMUNICATION_OWNERSHIP;
    packet.state.isInVisualKick = true;
    packet.ownership = {1, 2, 45, 1200};
    packet.session = 1234;
    packet.canControl = 1;
    check(decodeTeamPacket(&packet, sizeof(packet), decoded), "new packet roundtrip failed");
    check(decoded.session == 1234 && decoded.state.isInVisualKick &&
          sameDecision(packet.ownership, decoded.ownership), "new packet fields changed");
    check(!decodeTeamPacket(&packet, sizeof(packet) - 1, decoded), "truncated packet accepted");
    packet.canControl = 2;
    check(!decodeTeamPacket(&packet, sizeof(packet), decoded), "bad readiness accepted");
    packet.canControl = 1;
    packet.state.cost = std::numeric_limits<double>::quiet_NaN();
    check(!decodeTeamPacket(&packet, sizeof(packet), decoded), "NaN cost accepted");
    packet.state.cost = 4;
    std::array<unsigned char, sizeof(packet)> raw{};
    std::memcpy(raw.data(), &packet, sizeof(packet));
    raw[20] = 2;
    check(!decodeTeamPacket(raw.data(), raw.size(), decoded), "invalid native bool accepted");
    std::memcpy(raw.data(), &packet, sizeof(packet));
    raw[120] = 2;
    check(!decodeTeamPacket(raw.data(), raw.size(), decoded), "invalid kick bool accepted");

    TeamCommunicationMsg v2{};
    fillPacket(v2);
    v2.isInVisualKick = true;
    check(decodeTeamPacket(&v2, sizeof(v2), decoded) && decoded.state.isInVisualKick &&
          decoded.ownership.owner == 0 && decoded.canControl == 0, "v2 decode/grant failed");
    TeamCommunicationMsgLegacy legacy{};
    fillPacket(legacy);
    check(decodeTeamPacket(&legacy, sizeof(legacy), decoded) &&
          decoded.state.ballPosToField.y == 2 && !decoded.state.isInVisualKick, "legacy layout failed");
    TeamCommunicationMsgInsertedV1 inserted{};
    fillPacket(inserted);
    inserted.isInVisualKick = true;
    check(decodeTeamPacket(&inserted, sizeof(inserted), decoded) &&
          decoded.state.isInVisualKick && decoded.state.cmdId == 15, "inserted layout failed");
    inserted.validation = 999;
    check(!decodeTeamPacket(&inserted, sizeof(inserted), decoded), "unknown protocol accepted");
    check(!decodeTeamPacket(nullptr, 0, decoded), "null packet accepted");
}

void sharedBallTests() {
    // 前锋 2 看见己方禁区球，守门员 1 与前锋 3 看不见。
    // 接收机离球只有 0.36 m，旧距离门槛会拒绝这份报告。
    TeamOwnershipPacket packet{}, decoded{};
    fillPacket(packet.state);
    packet.state.validation = VALIDATION_COMMUNICATION_OWNERSHIP;
    packet.state.ballPosToField = {-3.2, 0.2, 0.0};
    packet.state.ballRange = 1.2;
    packet.session = 43;
    packet.canControl = 1;
    check(decodeTeamPacket(&packet, sizeof(packet), decoded), "ball report wire decode failed");
    const auto &m = decoded.state;
    std::vector<team_ball::Report> reports{{m.playerId, m.isAlive, m.ballDetected,
        m.ballLocationKnown, 100.0, m.ballConfidence, m.ballRange,
        m.ballPosToField.x, m.ballPosToField.y}};
    for (const Position receiver : std::vector<Position>{{-3.5, 0.0}, {-2.3, 1.0}}) {
        const int selected = team_ball::selectReport(reports, 800.0, 60.0, 9.0, 6.0);
        check(selected == 0, "near own-penalty ball was rejected for unseen teammate");
        near(reports[selected].x, -3.2, "shared field x changed between robots");
        near(reports[selected].y, 0.2, "shared field y changed between robots");
        const double range = std::hypot(reports[selected].x - receiver.x,
                                      reports[selected].y - receiver.y);
        check(range < 1.5, "regression fixture must reproduce old distance rejection");
        check(kick_strategy::goalieCanClaim(-3.2, 0.2, range, 9, 2, 5, 0.3, 2.2),
              "shared ball cannot qualify for own-penalty clearance");
        check(kick_strategy::clearanceAction(true, range, 1.2, true, true, false) ==
              kick_strategy::ClearanceAction::approach, "shared ball started a blind goalie kick");
    }
    check(kick_strategy::clearanceAction(false, 0.4, 1.2, true, true, false) ==
          kick_strategy::ClearanceAction::defend, "shared ball bypassed ownership");
    check(kick_strategy::clearanceAction(true, 0.4, 1.2, true, true, true) ==
          kick_strategy::ClearanceAction::kick, "reacquired local ball cannot be cleared");
    check(team_ball::freshLocal(true, false, 100, 800), "fresh local ball not preferred");
    check(!team_ball::freshLocal(true, true, 100, 800), "shared ball became a local observation");
    check(!team_ball::freshLocal(true, false, 801, 800), "stalled local ball kept priority");
    reports[0].ageMs = 801;
    check(team_ball::selectReport(reports, 800, 60, 9, 6) == -1, "stale packet refreshed shared ball");
    reports[0].ageMs = 100;
    reports[0].detected = false;
    check(team_ball::selectReport(reports, 800, 60, 9, 6) == -1, "relayed ball caused an endless echo");
    reports[0].detected = true;
    reports[0].confidence = 59;
    check(team_ball::selectReport(reports, 800, 60, 9, 6) == -1, "low-confidence ball accepted");
    reports[0].confidence = 90;
    reports[0].x = std::numeric_limits<double>::quiet_NaN();
    check(team_ball::selectReport(reports, 800, 60, 9, 6) == -1, "NaN shared position accepted");
    reports[0].x = 100;
    check(team_ball::selectReport(reports, 800, 60, 9, 6) == -1, "outside-field shared position accepted");
    reports[0].x = -3.2;
    reports.push_back({3, true, true, true, 100, 90, 0.5, -3.1, 0.2});
    check(team_ball::selectReport(reports, 800, 60, 9, 6) == 1,
          "closest visual observer did not win shared-ball selection");

    near(team_ball::approachSpeed(0.4, 0.6), 0, "shared approach walked into unseen ball");
    check(team_ball::approachSpeed(0.7, 0.6) >= 0.3, "shared approach stalls in gait dead zone");
    near(team_ball::approachSpeed(2.0, 0.6), 0.6, "shared approach exceeded speed cap");
    team_ball::Recovery recovery;
    check(recovery.canPursue(0, true, true, 0.4, -3.2, 0.2), "initial shared pursuit denied");
    check(recovery.canPursue(2490, true, true, 0.4, -3.2, 0.2), "shared recovery timed out early");
    check(!recovery.canPursue(2500, true, true, 0.4, -3.2, 0.2), "blind owner held ball indefinitely");
    check(!recovery.canPursue(4000, true, false, 0.4, -3.15, 0.2), "blind owner reclaimed unmoved ball");
    check(recovery.canPursue(4100, true, false, 1.0, -2.7, 0.2), "moved ball did not enable recovery");
    recovery.canPursue(4200, true, true, 0.4, -2.7, 0.2);
    check(!recovery.canPursue(6700, true, true, 0.4, -2.7, 0.2), "second recovery did not time out");
    check(recovery.canPursue(6800, false, true, 0.4, -2.7, 0.2), "local reacquisition stayed blocked");

    Network net;
    net.nodes[0].canControl = true; // 守门员通过共享球成为候选
    net.nodes[0].cost = 2.0;
    net.run(3000);
    check(net.owner() == 1, "shared-ball goalie received no majority grant");
    net.nodes[0].canControl = false; // 近球完成有限扫描仍不能重识别，资格随消息发布
    net.run(3000);
    check(net.owner() == 2, "blind goalie did not release to visible striker");
    // Network::run 每帧还检查同时主控和仍在退出动作的机器人总数 <= 1。
}

void sharedBallRecoveryTests() {
    using team_ball::Recovery;
    using team_ball::RecoveryPhase;
    using team_ball::RecoverySettings;
    const auto settings = RecoverySettings{}.sanitized();

    // 守门员持续刷新同一球的位置；消息更新和小幅定位抖动不能续租寻球计时。
    // 先对准目标，再完整覆盖中、左、右、上、下、中，最后释放主控资格。
    Recovery scan;
    for (int now = 0; now <= 2500; now += 10) {
        const double x = 1.0 + 0.05 * std::sin(now / 100.0);
        const double y = 0.03 * std::cos(now / 100.0);
        const bool eligible = scan.canPursue(now, true, true, 0.4, x, y, 0.0, true, settings);
        check(eligible == (now < 2500), "shared refresh/jitter renewed recovery timeout");
        if (now == 2500) break;
        check(scan.phase() == RecoveryPhase::scan, "near aligned owner did not scan");
        const auto offset = scan.headOffset(now, settings);
        check(std::isfinite(offset.yaw) && std::isfinite(offset.pitch) &&
              std::abs(offset.yaw) <= settings.scanYaw &&
              std::abs(offset.pitch) <= settings.scanPitch, "head scan exceeded configured bounds");
        if (now == 0 || now == 400 || now == 1650 || now == 2490) {
            near(offset.yaw, 0.0, "center scan yaw wrong");
            near(offset.pitch, 0.0, "center scan pitch wrong");
        } else if (now == 650) {
            near(offset.yaw, settings.scanYaw, "left scan missing");
            near(offset.pitch, 0.0, "left scan changed pitch");
        } else if (now == 900) {
            near(offset.yaw, -settings.scanYaw, "right scan missing");
            near(offset.pitch, 0.0, "right scan changed pitch");
        } else if (now == 1150) {
            near(offset.yaw, 0.0, "upper scan changed yaw");
            near(offset.pitch, -settings.scanPitch, "upper scan missing");
        } else if (now == 1400) {
            near(offset.yaw, 0.0, "lower scan changed yaw");
            near(offset.pitch, settings.scanPitch, "lower scan missing");
        }
    }
    check(scan.phase() == RecoveryPhase::released &&
          std::string(scan.phaseName()) == "released", "failed recovery did not expose released phase");
    check(!scan.canPursue(4000, true, false, 0.4, 1.04, 0.01),
          "non-owner reclaimed the failed stationary target");
    check(!scan.canPursue(4100, true, false, 0.4, 1.0, 0.0, 0.0, false),
          "stale shared target remained actionable");
    check(scan.phase() == RecoveryPhase::inactive, "stale report kept active head scanning");
    check(!scan.canPursue(4200, true, true, 0.4, 1.0, 0.0),
          "report outage erased failed-target latch");
    check(scan.canPursue(4300, true, true, 0.4, 1.5, 0.0),
          "significant ball movement did not unlock recovery");
    check(scan.canPursue(4400, false, true, 0.4, 1.5, 0.0),
          "local observation did not end shared recovery");
    check(scan.phase() == RecoveryPhase::inactive, "local vision kept shared scanning active");
    check(scan.canPursue(4500, true, true, 0.4, 1.0, 0.0),
          "local reacquisition did not clear old failure latch");

    // 大角度先转身；只有近球且对准后才消耗重识别预算。
    Recovery movement;
    check(movement.canPursue(0, true, true, 1.5, 1.0, 0.0, 1.0),
          "turning toward shared ball denied pursuit");
    check(movement.phase() == RecoveryPhase::turn, "large shared bearing did not request turn first");
    const auto turningHead = movement.headOffset(0, settings);
    near(turningHead.yaw, 0, "turn-first phase added head scan yaw");
    near(turningHead.pitch, 0, "turn-first phase added head scan pitch");
    check(movement.canPursue(10000, true, true, 1.5, 1.0, 0.0, 0.4),
          "distant turning/approach consumed near-ball recovery budget");
    check(movement.phase() == RecoveryPhase::approach, "aligned distant owner did not approach");
    check(movement.canPursue(10010, true, true, 0.4, 1.0, 0.0, 0.4) &&
          movement.phase() == RecoveryPhase::turn, "near misaligned owner started scanning too early");
    check(movement.canPursue(10020, true, true, 0.4, 1.0, 0.0, 0.2) &&
          movement.phase() == RecoveryPhase::scan, "aligned near owner did not enter scan");
    check(movement.canPursue(10120, true, true, 0.70, 1.0, 0.0, 0.3) &&
          movement.phase() == RecoveryPhase::scan, "range/bearing jitter restarted near-ball scan");
    check(movement.canPursue(10220, true, true, 0.80, 1.0, 0.0, 0.0) &&
          movement.phase() == RecoveryPhase::approach, "leaving scan hysteresis did not permit approach");
    check(movement.canPursue(20000, true, true, 0.4, 1.0, 0.0) &&
          movement.phase() == RecoveryPhase::scan, "return to near target did not resume scan");
    check(movement.canPursue(22290, true, true, 0.4, 1.0, 0.0),
          "approach interval consumed paused scan dwell");
    check(!movement.canPursue(22300, true, true, 0.4, 1.0, 0.0),
          "near/far movement repeatedly renewed fixed-target scan budget");

    Recovery misaligned;
    check(misaligned.canPursue(0, true, true, 0.4, 1.0, 0.0, 0.5) &&
          misaligned.phase() == RecoveryPhase::turn, "near misaligned target skipped turn phase");
    check(misaligned.canPursue(4990, true, true, 0.4, 1.0, 0.0, 0.5),
          "near turn exceeded failure budget before its bound");
    check(!misaligned.canPursue(5000, true, true, 0.4, 1.0, 0.0, 0.5),
          "perpetual near-ball misalignment held ownership indefinitely");

    // 仲裁完成后用本帧实际授权同步行为，接应者不消耗主控扫描预算。
    Recovery ownership;
    check(ownership.canPursue(0, true, false, 0.4, 1.0, 0.0) &&
          ownership.phase() == RecoveryPhase::support, "non-owner did not stay in support phase");
    check(ownership.canPursue(10000, true, false, 0.4, 1.0, 0.0),
          "support robot timed out without pursuing the ball");
    ownership.syncOwner(10000, true, 0.4, 0.0, settings);
    check(ownership.phase() == RecoveryPhase::scan, "new current-frame owner stayed in support phase");
    near(ownership.phaseElapsedMs(10000), 0.0, "new owner inherited support waiting time");
    ownership.syncOwner(11000, false, 0.4, 0.0, settings);
    check(ownership.phase() == RecoveryPhase::support, "lost ownership kept body in owner search phase");
    check(ownership.canPursue(20000, true, false, 0.4, 1.0, 0.0),
          "support interval consumed recovery budget");
    ownership.syncOwner(20000, true, 0.4, 0.0, settings);
    check(ownership.canPursue(21490, true, true, 0.4, 1.0, 0.0),
          "ownership interruption consumed paused recovery budget");
    check(!ownership.canPursue(21500, true, true, 0.4, 1.0, 0.0),
          "ownership handoff erased previous fixed-target scan dwell");

    Recovery syncOnly;
    syncOnly.canPursue(0, true, true, 0.4, 1.0, 0.0);
    syncOnly.syncOwner(3000, false, 0.4, 0.0, settings);
    check(syncOnly.phase() == RecoveryPhase::support &&
          syncOnly.canPursue(3000, true, false, 0.4, 1.0, 0.0),
          "post-arbitration ownership sync expired candidate eligibility");
    check(!syncOnly.canPursue(3100, true, true, 0.4, 1.0, 0.0),
          "pre-arbitration recovery update did not enforce exhausted scan budget");

    Recovery interrupted;
    interrupted.canPursue(0, true, true, 0.4, 1.0, 0.0);
    check(!interrupted.canPursue(1000, true, true, 0.4, 1.0, 0.0, 0.0, false) &&
          interrupted.phase() == RecoveryPhase::inactive, "stale report kept recovery active");
    check(interrupted.canPursue(10000, true, true, 0.4, 1.0, 0.0),
          "report outage consumed recovery budget");
    check(!interrupted.canPursue(11500, true, true, 0.4, 1.0, 0.0),
          "fresh report after outage renewed fixed-target recovery budget");

    // 配置再短也要完成一次扫描；异常配置及几何数据不能产生非法头部命令。
    RecoverySettings shortSettings;
    shortSettings.timeoutMs = 1.0;
    const auto shortScan = shortSettings.sanitized();
    check(shortScan.timeoutMs >= shortScan.scanDelayMs + 6 * shortScan.scanStepMs,
          "configured timeout truncates complete head scan");
    Recovery shortRecovery;
    shortRecovery.canPursue(0, true, true, 0.4, 1.0, 0.0, 0.0, true, shortSettings);
    check(shortRecovery.canPursue(shortScan.timeoutMs - 1, true, true, 0.4,
              1.0, 0.0, 0.0, true, shortSettings), "short configuration aborted scan early");
    check(!shortRecovery.canPursue(shortScan.timeoutMs, true, true, 0.4,
              1.0, 0.0, 0.0, true, shortSettings), "short configuration prevented finite release");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    RecoverySettings badSettings;
    badSettings.stopDistance = nan;
    badSettings.turnAngle = -1;
    badSettings.yawLimit = nan;
    badSettings.scanDelayMs = -1;
    badSettings.scanStepMs = inf;
    badSettings.scanYaw = nan;
    badSettings.scanPitch = -1;
    badSettings.timeoutMs = nan;
    badSettings.targetMoveDistance = -1;
    const auto safe = badSettings.sanitized();
    check(std::isfinite(safe.stopDistance) && safe.stopDistance > 0 &&
          std::isfinite(safe.turnAngle) && safe.turnAngle > 0 &&
          std::isfinite(safe.yawLimit) && safe.yawLimit > 0 &&
          std::isfinite(safe.scanDelayMs) && safe.scanDelayMs >= 0 &&
          std::isfinite(safe.scanStepMs) && safe.scanStepMs > 0 &&
          std::isfinite(safe.scanYaw) && safe.scanYaw >= 0 &&
          std::isfinite(safe.scanPitch) && safe.scanPitch >= 0 &&
          std::isfinite(safe.timeoutMs) && safe.timeoutMs >= safe.scanDelayMs + 6 * safe.scanStepMs &&
          std::isfinite(safe.targetMoveDistance) && safe.targetMoveDistance > 0,
          "invalid recovery configuration was not sanitized");
    near(team_ball::approachSpeed(0.8, 0.6, 0.9), 0.0,
          "shared approach ignored configured stop distance");
    near(team_ball::approachSpeed(2.0, nan), 0.0, "invalid speed cap produced shared motion");
    for (const auto input : std::vector<std::array<double, 5>>{
            {-1, 0.4, 1, 0, 0}, {nan, 0.4, 1, 0, 0}, {0, -1, 1, 0, 0},
            {0, inf, 1, 0, 0}, {0, 0.4, nan, 0, 0}, {0, 0.4, 1, nan, 0},
            {0, 0.4, 1, 0, nan}}) {
        Recovery invalid;
        check(!invalid.canPursue(input[0], true, true, input[1], input[2], input[3], input[4]) &&
              invalid.phase() == RecoveryPhase::inactive, "invalid geometry started shared pursuit");
        const auto offset = invalid.headOffset(0, badSettings);
        near(offset.yaw, 0, "invalid geometry generated head yaw");
        near(offset.pitch, 0, "invalid geometry generated head pitch");
    }

    // 将实际恢复资格接入延迟 UDP 仲裁：盲主攻扫描失败后让给有视觉的前锋。
    Network network;
    network.run(3000);
    check(network.owner() == 2, "recovery handoff fixture has no initial owner");
    Recovery blindOwner;
    for (int tick = 0; tick < 450; ++tick) {
        network.nodes[1].canControl = blindOwner.canPursue(network.now, true,
            network.nodes[1].owns, 0.4, 1.0, 0.0);
        network.run(10);
        blindOwner.syncOwner(network.now, network.nodes[1].owns, 0.4, 0.0, settings);
    }
    check(network.owner() == 3 && !network.nodes[1].canControl,
          "failed shared search did not hand ownership to visible striker");
}

void goalieRoleTests() {
    using goalie_strategy::Candidate;
    const auto protectedRole = [](bool goalie, bool known, bool kicking,
                                  double x, double range) {
        return goalie_strategy::protectClearanceRole(
            goalie, known, kicking, x, 0.0, range, 9.0, 2.0, 5.0, 0.3, 2.2);
    };
    // Reproduce a closer striker arriving during a goalkeeper SDK startup.
    // The old selection excluded every kicking player on the following tick,
    // switching the keeper to striker and halting the running behavior tree.
    for (int tick = 0; tick < 200; ++tick) {
        const bool kicking = tick >= 8;
        const std::vector<Candidate> team = {
            {1, 1.0, kicking, protectedRole(true, true, kicking, -3.5, 0.4)},
            {2, 0.3, false, false}, {3, 2.0, true, false},
        };
        check(goalie_strategy::select(team, 0.15) == 1,
              "goalkeeper role changed during clearance or SDK startup");
    }
    check(protectedRole(true, false, true, -3.5, 0.4),
          "brief detection loss changed goalkeeper during SDK exit");
    check(!protectedRole(false, true, true, -3.5, 0.4),
          "kicking striker was promoted to protected goalkeeper");
    check(!protectedRole(true, false, false, -3.5, 0.4),
          "unknown ball locked goalkeeper assignment indefinitely");
    check(!protectedRole(true, true, false, -3.5, 2.3),
          "distant ball prevented normal goalie election");
    check(!protectedRole(true, true, true, -1.8, 0.4),
          "attacking outside penalty area blocked goalkeeper handoff");
    check(goalie_strategy::select({{1, 0.5, true, false}, {2, 1.0, false, false}}, 0.15) == 2,
          "attacking player prevented another robot from covering goal");
    check(goalie_strategy::select({{3, 0.6, false, false}, {2, 0.5, false, false}}, 0.15) == 2,
          "normal goalkeeper tie did not choose deterministic player");
    // Adjacent pairs are within the tie margin but endpoints are not.
    // Each robot used to consider self first, producing different results.
    const std::vector<Candidate> overlappingTies = {
        {1, 0.30, false, false}, {2, 0.16, false, false}, {3, 0.05, false, false},
    };
    std::vector<int> order{0, 1, 2};
    do {
        const std::vector<Candidate> reordered = {
            overlappingTies[order[0]], overlappingTies[order[1]], overlappingTies[order[2]],
        };
        check(goalie_strategy::select(reordered, 0.15) == 2,
              "goalkeeper selection depends on self-first candidate order");
    } while (std::next_permutation(order.begin(), order.end()));
    check(goalie_strategy::select({{1, std::numeric_limits<double>::quiet_NaN(), false, true}}, 0.15) == 0,
          "invalid goalkeeper pose was accepted");
    check(goalie_strategy::select({}, 0.15) == 0, "empty goalkeeper list selected a player");
}

void readyFormationTests() {
    // 每台只凭相同裁判机名单计算，通信中断、距离排名变化不能抢同一站位。
    for (int goalkeeper = 1; goalkeeper <= 3; ++goalkeeper) {
        std::array<ready_formation::Assignment, 3> robots;
        for (auto &robot : robots) robot.update(true, goalkeeper, 3);
        int expectedRank = 0;
        for (int player = 1; player <= 3; ++player) {
            for (auto &robot : robots) {
                check(robot.goalkeeper() == goalkeeper, "READY goalkeeper differs between robots");
                check(robot.strikerRank(player) == (player == goalkeeper ? -1 : expectedRank),
                      "READY striker slots do not follow player ID");
            }
            if (player != goalkeeper) ++expectedRank;
        }
        // 同一轮不受后来的名单/动态角色输入扰动；下一轮允许使用新名单。
        const int changedGoalkeeper = goalkeeper % 3 + 1;
        robots[0].update(true, changedGoalkeeper, 2);
        check(robots[0].goalkeeper() == goalkeeper, "READY changed slots mid-cycle");
        robots[0].update(false, changedGoalkeeper, 3);
        check(!robots[0].active() && robots[0].strikerRank(2) == -1,
              "leaving READY kept its assignment active");
        robots[0].update(true, changedGoalkeeper, 3);
        check(robots[0].goalkeeper() == changedGoalkeeper, "new READY did not reset assignment");
    }
    ready_formation::Assignment fallback;
    fallback.update(true, 0, 3);
    check(fallback.goalkeeper() == 1 && fallback.strikerRank(2) == 0 &&
          fallback.strikerRank(3) == 1, "unknown GC goalkeeper has inconsistent fallback");
    check(fallback.strikerRank(0) == -1 && fallback.strikerRank(4) == -1,
          "invalid player ID was allocated a READY slot");

    struct Field { double length, width, circleRadius; };
    for (const auto field : {Field{9, 6, 0.75}, Field{14.16, 9.22, 1.54}}) {
        for (bool ourKickoff : {true, false}) {
            const auto front = ready_formation::strikerTarget(0, ourKickoff, field);
            const auto support = ready_formation::strikerTarget(1, ourKickoff, field);
            check(front.x < 0 && front.y == 0 && support.x < front.x && support.y < 0,
                  "READY center/support layout is wrong");
            // 两台都在 0.5m 解锁容差的最靠近侧时，仍超过默认 0.8m 停止距离。
            check(std::hypot(front.x - support.x, front.y - support.y) - 2 * 0.5 > 0.8,
                  "READY arrival windows overlap teammate stop distance");
            check(std::abs(support.y) + 0.5 < field.width / 2 &&
                  support.x - 0.5 > -field.length / 2 && front.x + 0.5 < 0,
                  "READY tolerance permits crossing field boundaries/middle line");
            if (!ourKickoff) {
                check(-front.x - 0.5 - 0.25 > field.circleRadius,
                      "defending READY tolerance enters center circle");
            }
        }
    }
    const auto front = ready_formation::strikerTarget(0, true, Field{9, 6, 0.75});
    near(front.x, -1.15, "kid-size center READY target wrong");
    const auto support = ready_formation::strikerTarget(1, true, Field{9, 6, 0.75});
    near(support.x, -2.15, "kid-size support READY depth wrong");
    near(support.y, -1.8, "kid-size support READY side wrong");
    const Position lower{-2.0, -0.1}, higher{-2.0, -0.85};
    const auto escape = yieldVelocity(higher, lower, {front.x, front.y},
        {{lower.x - higher.x, lower.y - higher.y}}, 9.0, 6.0, 0.8, 1.3, 0.4, -1);
    check(std::hypot(escape.x, escape.y) > 0.05 &&
          std::hypot(higher.x + escape.x - lower.x, higher.y + escape.y - lower.y) > 0.75,
          "higher-ID striker cannot yield at READY route conflict");
}

void geometryTests() {
    near(segmentDistance({-1, 0}, {1, 0}, {0, -1}, {0, 1}), 0, "crossing routes missed");
    near(segmentDistance({0, 0}, {1, 0}, {2, 0}, {3, 0}), 1, "collinear gap wrong");
    near(pointSegmentDistance({1, 1}, {0, 0}, {0, 0}), std::sqrt(2), "zero-length segment wrong");
    check(supportSide({-1, 1.5}, {-1.4, 1.2}, {-1.4, -1.2},
          {-0.5, 0}, {0, 0}, 0.8, 1) == 1, "supporter crossed owner corridor");
    check(supportSide({-2, 0}, {-1.4, 1.2}, {-1.4, -1.2},
          {-0.5, 0}, {0, 0}, 0.8, -1) == -1, "support side tie was not stable");
    double vx = 1, vy = 0.2;
    limitApproach(vx, vy, {0.6, 0}, 0.8, 1.3, 0.4);
    near(vx, 0, "close teammate allowed forward motion");
    near(vy, 0.2, "safe side motion was blocked");
    vx = -0.5;
    limitApproach(vx, vy, {0.6, 0}, 0.8, 1.3, 0.4);
    near(vx, -0.5, "retreat was blocked");
    vx = 1; vy = 0;
    limitApproach(vx, vy, {1.05, 0}, 0.8, 1.3, 0.4);
    near(vx, 0.2, "slow-zone approach speed wrong");
    vx = 1; vy = 0.5;
    limitApproach(vx, vy, {0, 0}, 0.8, 1.3, 0.4);
    near(vx, 0, "overlapping pose did not stop motion");
    std::mt19937 random{21};
    std::uniform_real_distribution<double> position(-1.3, 1.3), speed(-2, 2);
    for (int i = 0; i < 20000; ++i) {
        const std::vector<Position> teammates{{position(random), position(random)},
                                             {position(random), position(random)}};
        vx = speed(random); vy = speed(random); double omega = 1;
        limitTeamVelocity(vx, vy, omega, teammates, 0.8, 1.3, 0.4, 0.6, 0.3);
        check(std::fabs(vx) <= 0.6 + 1e-8 && std::fabs(vy) <= 0.3 + 1e-8,
              "teammate projection exceeded command speed caps");
        for (const auto &p : teammates) {
            double checkedX = vx, checkedY = vy;
            limitApproach(checkedX, checkedY, p, 0.8, 1.3, 0.4);
            check(std::hypot(checkedX - vx, checkedY - vy) <= 1.1e-5,
                  "multiple teammates or final caps reintroduced unsafe approach");
            if (std::hypot(p.x, p.y) < 0.8) near(omega, 0, "close teammate allowed rotation");
        }
    }
}

void clearanceLivenessTests() {
    Network net;
    net.run(3000);
    check(net.owner() == 2, "initial striker owner missing");
    // 守门员调整朝向的成本更高，但已直接看见脚边球；共享球的
    // 旧主控不能一直持有授权并停在 0.65 m 外等待自己的视觉。
    net.nodes[0].canControl = true;
    net.nodes[0].nearVisibleBall = true;
    net.nodes[0].cost = 9.0;
    net.run(1800);
    check(net.owner() == 1, "visible near-ball goalie lost to blind lower-cost striker");
    net.nodes[0].kicking = true;
    net.nodes[1].nearVisibleBall = true;
    net.nodes[1].cost = 1.0;
    net.run(2500);
    check(net.owner() == 1, "cluster interrupted active owner's kick");

    const Position ball{0.35, 0.0};
    check(!blocksKick({-0.65, 0.0}, ball, 0.0, 0.8), "rear teammate blocked forward clearance");
    check(!blocksKick({0.0, 0.65}, ball, 0.0, 0.8), "side teammate blocked clear kick corridor");
    check(blocksKick({0.65, 0.0}, ball, 0.0, 0.8), "forward teammate did not block kick");
    check(blocksKick({-0.3, 0.0}, ball, 0.0, 0.8), "body contact zone did not stop kick");
    check(blocksKick({0.0, 0.65}, {0.0, 0.35}, std::acos(-1.0) / 2.0, 0.8),
          "rotated kick corridor ignored side teammate");

    // 禁区内三台聚集：非主控守门员也必须退出拦截逻辑主动让行。
    // 两个让行者同时朝两侧离开，不能只验证它们最终目标点安全。
    const Position owner{-3.8, 0.0}, fieldBall{-3.3, 0.0};
    std::array<Position, 2> supporters{{{-3.5, 0.2}, {-3.55, -0.2}}};
    for (int tick = 0; tick < 100; ++tick) {
        const auto before = supporters;
        for (int i = 0; i < 2; ++i) {
            const auto self = before[i], other = before[1 - i];
            if (!mustYield(self, owner, fieldBall, 1.15)) continue;
            const std::vector<Position> others{{owner.x - self.x, owner.y - self.y},
                {other.x - self.x, other.y - self.y}};
            const auto velocity = yieldVelocity(self, owner, fieldBall, others,
                9.0, 6.0, 0.8, 1.3, 0.4, i == 0 ? 1 : -1);
            check(std::hypot(velocity.x, velocity.y) > 0.1,
                  "cluster left safe yielding robot stationary");
            supporters[i] = {self.x + velocity.x * 0.1, self.y + velocity.y * 0.1};
            check(std::hypot(supporters[i].x - owner.x, supporters[i].y - owner.y) >=
                std::hypot(self.x - owner.x, self.y - owner.y), "yield approached owner");
        }
        check(std::hypot(supporters[0].x - supporters[1].x,
                         supporters[0].y - supporters[1].y) >= 0.4 - 1e-8,
              "simultaneous yield moved teammates into each other");
    }
    for (const auto &p : supporters) {
        check(!mustYield(p, owner, fieldBall, 1.15), "yield never cleared goalie corridor");
    }
    const Position boundary{-4.25, 0.1};
    const auto escape = yieldVelocity(boundary, owner, fieldBall,
        {{owner.x - boundary.x, owner.y - boundary.y}}, 9.0, 6.0, 0.8, 1.3, 0.4, 1);
    check(std::hypot(escape.x, escape.y) > 0.1 && boundary.x + escape.x >= -4.3,
          "goal-line yielding was clipped into a permanent wait");
}
} // namespace

int main() {
    try {
        arbitrationTests();
        packetTests();
        sharedBallTests();
        sharedBallRecoveryTests();
        goalieRoleTests();
        readyFormationTests();
        geometryTests();
        clearanceLivenessTests();
        std::cout << "Team cooperation: arbitration, shared ball recovery, goalie roles, READY formation, packet decoding and geometry passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
