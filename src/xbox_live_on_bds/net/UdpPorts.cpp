#include "xbox_live_on_bds/net/UdpPorts.h"

#include <algorithm>
#include <charconv>
#include <fstream>

#include <fmt/format.h>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace xbox_live_on_bds::net {

namespace {

std::optional<std::uint16_t> parsePort(std::string_view text) {
    unsigned value{};
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value == 0 || value > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

/// A server.properties value; empty when the key is missing.
std::string readProperty(std::filesystem::path const& file, std::string_view key) {
    std::ifstream in(file);
    std::string   line;
    std::string   value;
    while (std::getline(in, line)) {
        std::string_view view = trim(line);
        if (view.starts_with('#')) continue;
        auto equals = view.find('=');
        if (equals == std::string_view::npos || trim(view.substr(0, equals)) != key) continue;
        value = std::string(trim(view.substr(equals + 1)));
    }
    return value;
}

} // namespace

std::optional<PortRange> localUdpPorts(std::string_view value) {
    value = trim(value);
    if (value.starts_with('[')) { // [IPv6]:public:local
        auto close = value.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        value.remove_prefix(close + 1);
    }
    auto colon = value.rfind(':');
    if (colon != std::string_view::npos) value.remove_prefix(colon + 1);
    auto dash  = value.find('-');
    auto first = parsePort(value.substr(0, dash));
    auto last  = dash == std::string_view::npos ? first : parsePort(value.substr(dash + 1));
    if (!first || !last || *last < *first) return std::nullopt;
    return PortRange{*first, *last};
}

bool udpPortInUse(std::uint16_t port) {
    static bool const started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started) return false;
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    bool inUse           = false;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int error = WSAGetLastError();
        inUse     = error == WSAEADDRINUSE || error == WSAEACCES;
    }
    closesocket(s);
    return inUse;
}

std::optional<std::string> checkServerUdpPorts(std::filesystem::path const& serverProperties) {
    std::string transport = readProperty(serverProperties, "transport");
    if (!transport.empty() && transport != "nethernet") return std::nullopt; // RakNet ignores server-udp-ports.
    std::string value = readProperty(serverProperties, "server-udp-ports");
    if (value.empty()) return std::nullopt; // The OS picks free ports.
    auto range = localUdpPorts(value);
    if (!range) return std::nullopt;
    // A range only needs one free port; a huge one is not worth probing in full.
    unsigned last = std::min<unsigned>(range->last, range->first + 1023u);
    for (unsigned port = range->first; port <= last; ++port) {
        if (!udpPortInUse(static_cast<std::uint16_t>(port))) return std::nullopt;
    }
    return fmt::format(
        "server-udp-ports={} is already in use by another program (another server?): this server cannot reach "
        "players over NetherNet, and friends will hang at \"Connecting\". Give each server its own UDP port.",
        value
    );
}

} // namespace xbox_live_on_bds::net
