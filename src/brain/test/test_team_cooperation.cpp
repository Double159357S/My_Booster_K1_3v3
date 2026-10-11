#include <array>
#include <iostream>
#include <random>
#include <stdexcept>

#include "team_communication_msg.h"
#include "utils/team_ball.h"
#include "utils/kick_strategy.h"
#include "utils/goalie_strategy.h"

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
                    n.cost, 0.0, n.arbitration.announcement(now)}};
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
                         n.cost, now + 1.0, n.arbitration.announcement(now)}});
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
    check(recovery.canPursue(1990, true, true, 0.4, -3.2, 0.2), "shared recovery timed out early");
    check(!recovery.canPursue(2000, true, true, 0.4, -3.2, 0.2), "blind owner held ball indefinitely");
    check(!recovery.canPursue(4000, true, false, 0.4, -3.15, 0.2), "blind owner reclaimed unmoved ball");
    check(recovery.canPursue(4100, true, false, 1.0, -2.7, 0.2), "moved ball did not enable recovery");
    recovery.canPursue(4200, true, true, 0.4, -2.7, 0.2);
    check(!recovery.canPursue(6200, true, true, 0.4, -2.7, 0.2), "second recovery did not time out");
    check(recovery.canPursue(6300, false, true, 0.4, -2.7, 0.2), "local reacquisition stayed blocked");

    Network net;
    net.nodes[0].canControl = true; // 守门员通过共享球成为候选
    net.nodes[0].cost = 2.0;
    net.run(3000);
    check(net.owner() == 1, "shared-ball goalie received no majority grant");
    net.nodes[0].canControl = false; // 近球 2 s 仍不能重识别，资格随消息发布
    net.run(3000);
    check(net.owner() == 2, "blind goalie did not release to visible striker");
    // Network::run 每帧还检查同时主控和仍在退出动作的机器人总数 <= 1。
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
} // namespace

int main() {
    try {
        arbitrationTests();
        packetTests();
        sharedBallTests();
        goalieRoleTests();
        geometryTests();
        std::cout << "Team cooperation: arbitration, shared ball, goalie roles, packet decoding and geometry passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
