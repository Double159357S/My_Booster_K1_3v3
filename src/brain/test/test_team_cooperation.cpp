#include <array>
#include <iostream>
#include <random>
#include <stdexcept>

#include "team_communication_msg.h"

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
    Network pair;
    pair.nodes[0].alive = false;
    pair.isolate(1, true);
    pair.run(3500);
    check(pair.owner() == 2, "two strikers cannot elect without goalie");
    pair.nodes[2].cost = 1.0;
    pair.run(3500);
    check(pair.owner() == 3, "two strikers cannot hand off ownership");
    pair.isolate(2, true);
    pair.run(2000);
    check(pair.owner() == 0, "disconnected pair retains ownership");

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
        geometryTests();
        std::cout << "Team cooperation: arbitration, packet decoding and geometry passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
