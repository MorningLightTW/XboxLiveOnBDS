#include "xbox_live_on_bds/ice/StandIn.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "xbox_live_on_bds/ice/Stun.h"

namespace xbox_live_on_bds::ice {

namespace {
void ensureWinsock() {
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}
} // namespace

std::unique_ptr<StandIn> StandIn::open(std::string remoteUfrag, std::string remotePassword) {
    ensureWinsock();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return nullptr;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0;
    int len              = sizeof(addr);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::closesocket(s);
        return nullptr;
    }
    DWORD timeoutMs = 200; // so the thread notices mStop
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&timeoutMs), sizeof(timeoutMs));

    std::unique_ptr<StandIn> standIn(new StandIn());
    standIn->mSocket   = s;
    standIn->mPort     = ntohs(addr.sin_port);
    standIn->mUfrag    = std::move(remoteUfrag);
    standIn->mPassword = std::move(remotePassword);
    standIn->mThread   = std::thread([raw = standIn.get()] { raw->run(); });
    return standIn;
}

StandIn::~StandIn() {
    mStop = true;
    if (mThread.joinable()) mThread.join();
    ::closesocket(static_cast<SOCKET>(mSocket));
}

void StandIn::setOnOtherPacket(std::function<void(std::string_view)> handler) { mOnOther = std::move(handler); }

void StandIn::run() {
    char buffer[2048];
    while (!mStop) {
        sockaddr_in from{};
        int         fromLen = sizeof(from);
        int         n       = ::recvfrom(
            static_cast<SOCKET>(mSocket),
            buffer,
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen
        );
        if (n <= 0) continue; // timeout, or an ICMP-induced error
        std::string_view packet(buffer, static_cast<std::size_t>(n));

        auto header = stun::parseHeader(packet);
        if (header && header->type == stun::BindingRequest) {
            // BDS checking the friend: USERNAME is "<friend ufrag>:<BDS ufrag>", signed with the friend's password.
            auto user = stun::findAttribute(packet, stun::Username);
            if (!user || !user->starts_with(mUfrag + ":")) continue;
            auto reply = stun::bindingSuccess(*header, from.sin_addr.s_addr, ntohs(from.sin_port), mPassword);
            ::sendto(
                static_cast<SOCKET>(mSocket),
                reply.data(),
                static_cast<int>(reply.size()),
                0,
                reinterpret_cast<sockaddr*>(&from),
                fromLen
            );
            ++mAnswered;
            continue;
        }
        if (mOnOther) mOnOther(packet);
    }
}

std::string primaryIpv4() {
    ensureWinsock();
    // "Connecting" a UDP socket sends nothing; it only picks the route, and so the local address.
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port   = htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    std::string result;
    if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in local{};
        int         len = sizeof(local);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char text[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
            result = text;
        }
    }
    ::closesocket(s);
    return result;
}

} // namespace xbox_live_on_bds::ice
