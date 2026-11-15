#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ll::io {
class Logger;
}

namespace xbox_live_on_bds::xbl {

/// The host side of Minecraft's friend-join signaling, carried into BDS's own NetherNet:
///
///   friend's game --(Xbox signaling: CONNECTREQUEST / CANDIDATEADD)--> this --(inject)--> BDS's transport
///   BDS's transport --(CONNECTRESPONSE / CANDIDATEADD, caught by the sink)--> this --> friend's game
///
/// after which the game and BDS talk WebRTC directly, or through Microsoft's TURN relays. Each join also gets a
/// local stand-in candidate (ice::StandIn) so BDS releases its answer before it can reach a friend behind NAT.
/// If the transport cannot be reached, a join falls back to BDS's HTTP signaling (POST /v1/join/<id>), which
/// only works when BDS can reach the friend directly. Service:
/// wss://signal.franchise.minecraft-services.net/ws/v1.0/messaging/connect, authenticated with the host
/// account's MCToken, which also gives the player messaging id (PmsgId) the lobby session advertises.
class Signaling {
public:
    struct TurnServer {
        std::vector<std::string> urls;
        std::string              username;
        std::string              password;
    };

    struct Options {
        std::string           minecraftAuthorization; // "MCToken ..."
        std::uint64_t         netherNetId = 0;        // BDS's NetherNet id: the one the lobby session advertises.
        std::string           relayBaseUrl;           // BDS's HTTP signaling, e.g. "http://127.0.0.1:19132".
        std::filesystem::path debugDir;               // When set, the last relayed offer and answer are written there.
        /// Hand a friend's signal ("<TYPE> <connection id> <payload>") to BDS's NetherNet transport; false when it
        /// cannot be reached, and the join goes through BDS's HTTP signaling instead.
        std::function<bool(std::uint64_t from, std::string_view signal)> inject;
        /// Register (or clear, with an empty function) the receiver of the transport's signals to those friends.
        std::function<void(std::function<void(std::uint64_t to, std::string signal)>)> setInjectSink;
        /// The TURN relays (and STUN) the signaling service hands out, for the server to use; called once connected.
        std::function<void(std::vector<TurnServer>)> onTurnServers;
        /// Replacing a connection that dropped: its TURN grants are no news and log at debug.
        bool reconnect = false;
        /// Called once, from the signaling thread, if the connection drops or goes silent, with why.
        std::function<void(std::string const& reason)> onClosed;
    };

    /// Connects; nullptr on failure, with the reason in `error`.
    static std::unique_ptr<Signaling> connect(ll::io::Logger& logger, Options options, std::string& error);

    ~Signaling();

    [[nodiscard]] bool isOpen() const;

private:
    struct Impl;
    explicit Signaling(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> mImpl;
};

} // namespace xbox_live_on_bds::xbl
