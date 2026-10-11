#include "utils/team_network.h"
#include <unistd.h>
#include <cassert>
#include <iostream>

int main()
{
    auto targets = team_network::broadcastTargets(10070, "192.168.10.255");
    assert(targets.size() == 1 && ntohs(targets[0].sin_port) == 10070);
    assert(targets[0].sin_addr.s_addr == inet_addr("192.168.10.255"));
    bool rejected = false;
    try { team_network::broadcastTargets(10070, "not-an-ip"); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    for (const auto &target : team_network::broadcastTargets(10070, "")) {
        assert(target.sin_family == AF_INET && ntohs(target.sin_port) == 10070);
        assert(target.sin_addr.s_addr != inet_addr("127.0.0.1"));
    }

    // Exercise generated destination addresses through real UDP, on an ephemeral port.
    const int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    const int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0);
    socklen_t length = sizeof(local);
    assert(getsockname(receiver, reinterpret_cast<sockaddr *>(&local), &length) == 0);
    timeval timeout{1, 0};
    assert(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    targets = team_network::broadcastTargets(ntohs(local.sin_port), "127.0.0.1");
    const char message[] = "team UDP";
    assert(sendto(sender, message, sizeof(message), 0,
        reinterpret_cast<const sockaddr *>(&targets[0]), sizeof(targets[0])) == sizeof(message));
    char received[32]{};
    assert(recv(receiver, received, sizeof(received), 0) == sizeof(message));
    assert(std::string(received) == message);
    close(receiver);
    close(sender);
    std::cout << "Team network: address selection and UDP delivery passed\n";
}
