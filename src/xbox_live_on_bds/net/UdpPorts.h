#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xbox_live_on_bds::net {

/// The local UDP ports a `server-udp-ports` value asks for: "19489", "29489:19489", "19400-19500",
/// "21200-21300:19400-19500", "203.0.113.10:19489:19489" or "[2001:db8::1]:21200-21300:19400-19500" (the last
/// part is always the local side). Empty for an empty or malformed value.
struct PortRange {
    std::uint16_t first = 0;
    std::uint16_t last  = 0;
};
std::optional<PortRange> localUdpPorts(std::string_view value);

/// Whether another socket already holds this UDP port on IPv4 (a bind of our own fails).
bool udpPortInUse(std::uint16_t port);

/// Before the server binds them: a warning if server.properties gives NetherNet UDP ports that other programs
/// already hold. BDS then gathers no ICE candidates and friends hang at "Connecting", without a word in its log.
std::optional<std::string> checkServerUdpPorts(std::filesystem::path const& serverProperties);

} // namespace xbox_live_on_bds::net
