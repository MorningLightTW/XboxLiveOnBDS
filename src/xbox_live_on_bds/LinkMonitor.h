#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mc/platform/UUID.h"

namespace ll::io {
class Logger;
}
namespace ll::data {
class CancellableCallback;
}
class Player;

namespace xbox_live_on_bds {

/// How each player is connected, sampled from BDS's own figures for the whole time they play: straight to this
/// machine or through Microsoft's TURN relays, ping and packet loss. A path change is logged when it happens,
/// a summary (ping spread, jitter, loss, time on a relay) when the player leaves. Server thread only.
class LinkMonitor {
public:
    explicit LinkMonitor(ll::io::Logger& logger);
    ~LinkMonitor();

    void start();
    void stop();

    void onJoin(Player const& player);
    void onLeave(Player const& player);
    /// The server is shutting down: summarize everyone still on, as no leave event follows.
    void onServerStopping();

    /// What the samples so far say about a player still on (nothing for one not tracked).
    struct Stats {
        double                minutes = 0;      // since joining
        std::optional<double> jitter;           // ms; needs two samples
        int                   relayPercent = 0; // of the samples
    };
    [[nodiscard]] std::optional<Stats> stats(Player const& player) const;

private:
    struct Link {
        mce::UUID                             uuid;
        std::string                           name;
        std::chrono::steady_clock::time_point joined;
        std::vector<long long>                pings; // ms, one per sample
        float                                 maxLoss      = 0;
        int                                   relaySamples = 0;
        bool                                  lastRelay    = false;
        bool                                  reported     = false;
    };

    void sampleAll();
    void schedule();
    void summarize(Link const& link) const;

    ll::io::Logger&                                mLogger;
    std::map<std::string, Link>                    mLinks; // by UUID text
    std::shared_ptr<ll::data::CancellableCallback> mTimer;
    bool                                           mRunning = false;
};

} // namespace xbox_live_on_bds
