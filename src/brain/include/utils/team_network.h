#pragma once

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace team_network
{
inline std::vector<sockaddr_in> broadcastTargets(int port, const std::string &overrideAddress)
{
    std::vector<sockaddr_in> targets;
    auto add = [&](in_addr address) {
        if (std::any_of(targets.begin(), targets.end(), [&](const sockaddr_in &target) {
            return target.sin_addr.s_addr == address.s_addr;
        })) return;
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        target.sin_addr = address;
        targets.push_back(target);
    };
    if (!overrideAddress.empty()) {
        in_addr address{};
        if (inet_pton(AF_INET, overrideAddress.c_str(), &address) != 1)
            throw std::runtime_error("Invalid team broadcast IPv4 address: " + overrideAddress);
        add(address);
        return targets;
    }
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0)
        throw std::runtime_error("Cannot enumerate team network interfaces");
    for (auto *iface = interfaces; iface; iface = iface->ifa_next) {
        if (!iface->ifa_addr || !iface->ifa_broadaddr ||
            iface->ifa_addr->sa_family != AF_INET ||
            !(iface->ifa_flags & IFF_UP) || !(iface->ifa_flags & IFF_BROADCAST) ||
            (iface->ifa_flags & IFF_LOOPBACK)) continue;
        add(reinterpret_cast<sockaddr_in *>(iface->ifa_broadaddr)->sin_addr);
    }
    freeifaddrs(interfaces);
    if (targets.empty()) throw std::runtime_error("No active IPv4 broadcast interface for team communication");
    return targets;
}
} // namespace team_network
