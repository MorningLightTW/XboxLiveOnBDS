// server-udp-ports parsing and the port-in-use check, offline: xmake build udp-ports-test && xmake run udp-ports-test
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <winsock2.h>

#include "xbox_live_on_bds/net/UdpPorts.h"

using namespace xbox_live_on_bds::net;

namespace {

bool is(std::optional<PortRange> range, unsigned first, unsigned last) {
    return range && range->first == first && range->last == last;
}

/// Holds an IPv4 UDP port the OS picks, as another server would; returns it (0 on failure).
std::uint16_t holdPort(SOCKET& s) {
    s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s == INVALID_SOCKET || ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return 0;
    int length = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &length);
    return ntohs(addr.sin_port);
}

} // namespace

int main() {
    int  failures = 0;
    auto check    = [&](bool ok, char const* what) {
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    };

    // The forms server.properties documents; the local side is always the last part.
    check(is(localUdpPorts("19489"), 19489, 19489), "single port");
    check(is(localUdpPorts("29489:19489"), 19489, 19489), "public:local");
    check(is(localUdpPorts("19400-19500"), 19400, 19500), "range");
    check(is(localUdpPorts("21200-21300:19400-19500"), 19400, 19500), "public range:local range");
    check(is(localUdpPorts("203.0.113.10:19489:19489"), 19489, 19489), "IPv4:public:local");
    check(is(localUdpPorts("[2001:db8::2021:26:03]:21200-21300:19400-19500"), 19400, 19500), "[IPv6]:public:local");
    check(is(localUdpPorts(" 19132\r"), 19132, 19132), "surrounding whitespace");
    check(!localUdpPorts(""), "empty");
    check(!localUdpPorts("abc"), "not a number");
    check(!localUdpPorts("70000"), "out of range");
    check(!localUdpPorts("19500-19400"), "backwards range");

    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    SOCKET held = INVALID_SOCKET;
    auto   port = holdPort(held);
    check(port != 0, "hold a port");
    check(udpPortInUse(port), "a held port is in use");

    auto file  = std::filesystem::temp_directory_path() / "xbox-live-on-bds-udp-ports-test.properties";
    auto write = [&](std::string const& text) { std::ofstream(file, std::ios::trunc) << text; };
    write("# server-udp-ports=1\ntransport=nethernet\nserver-udp-ports=" + std::to_string(port) + "\n");
    check(checkServerUdpPorts(file).has_value(), "warns when the server's port is held");
    write("transport=raknet\nserver-udp-ports=" + std::to_string(port) + "\n");
    check(!checkServerUdpPorts(file), "quiet for RakNet");
    write("server-udp-ports=\n");
    check(!checkServerUdpPorts(file), "quiet when the OS picks the ports");
    write("server-udp-ports=" + std::to_string(port) + "-" + std::to_string(port + 1) + "\n");
    // Only the first port is held, so the range still has a free one (unless something else holds port + 1).
    check(udpPortInUse(port + 1) || !checkServerUdpPorts(file), "quiet when a range has a free port");

    closesocket(held);
    check(!udpPortInUse(port), "a released port is free");
    std::filesystem::remove(file);
    WSACleanup();

    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
