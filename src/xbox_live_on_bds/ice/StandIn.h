#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace xbox_live_on_bds::ice {

/// A local UDP endpoint the server is told a joining friend can also be reached at. BDS answers a join only once
/// it can reach the joiner, which a friend behind NAT never allows before having that answer; the stand-in
/// answers BDS's connectivity checks for the friend (it knows the friend's ICE credentials from the offer), so
/// BDS lets the answer go.
class StandIn {
public:
    /// nullptr if no socket could be opened. `remoteUfrag`/`remotePassword`: the friend's a=ice-ufrag / a=ice-pwd.
    static std::unique_ptr<StandIn> open(std::string remoteUfrag, std::string remotePassword);
    ~StandIn();

    [[nodiscard]] std::uint16_t port() const { return mPort; }
    [[nodiscard]] int           checksAnswered() const { return mAnswered.load(); }

    /// Called (from the stand-in's thread) for each packet BDS sends it that is not a connectivity check.
    void setOnOtherPacket(std::function<void(std::string_view packet)> handler);

private:
    StandIn() = default;
    void run();

    std::uintptr_t                        mSocket{};
    std::uint16_t                         mPort{};
    std::string                           mUfrag;
    std::string                           mPassword;
    std::atomic<int>                      mAnswered{0};
    std::atomic<bool>                     mStop{false};
    std::function<void(std::string_view)> mOnOther;
    std::thread                           mThread;
};

/// The machine's IPv4 address on its default route (what BDS lists first), as dotted text; empty if none.
std::string primaryIpv4();

} // namespace xbox_live_on_bds::ice
