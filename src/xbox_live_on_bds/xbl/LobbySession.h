#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

#include "xbox_live_on_bds/xbl/Signaling.h"
#include <memory>
#include <string>

namespace ll::io {
class Logger;
}

namespace xbox_live_on_bds {
namespace auth {
class XboxAuth;
}

namespace xbl {

/// Who sees the session in the Friends tab: Minecraft's BroadcastSetting (NoMultiPlay 0, InviteOnly 1, FriendsOnly 2,
/// FriendsOfFriends 3, Public 4, in the order the game declares them).
enum class Visibility { Friends = 2, FriendsOfFriends = 3 };

/// What friends see in the Friends tab, and how their game reaches the server.
struct LobbyInfo {
    Visibility            visibility = Visibility::FriendsOfFriends;
    std::string           worldName; // Shown under the host's name (the host is always the signed-in account).
    std::string           version;   // Game version, e.g. "26.60.6".
    int                   protocol    = 0;
    std::string           gameMode    = "Survival"; // Survival, Creative or Adventure.
    int                   players     = 0;
    int                   maxPlayers  = 10;
    std::uint64_t         netherNetId = 0;   // The server's NetherNet id; joins are signaled to it.
    std::string           pmsgId;            // Minecraft player-messaging id of the host account (signaling).
    std::uint16_t         localPort = 19132; // BDS's port: its HTTP signaling (TCP) answers on it.
    std::filesystem::path debugDir;          // Where signaling writes the last relayed offer/answer (empty: nowhere).
                                             // Debugging only: they hold the joining player's identity token and IPs.
    // BDS's NetherNet transport, when reachable: friends' joins go straight into it (see Signaling::Options).
    std::function<bool(std::uint64_t from, std::string_view signal)>               inject;
    std::function<void(std::function<void(std::uint64_t to, std::string signal)>)> setInjectSink;
    std::function<void(std::vector<Signaling::TurnServer>)>                        onTurnServers;
    /// Makes `inject` work before the first friend joins; run on the session's own thread before each first publish.
    /// Returns false when the server must not be published (its NetherNet internals are not the ones this mod
    /// knows): the session then never connects and does not retry.
    std::function<bool()> prepareTransport;
};

/// Keeps a joinable-by-friends MinecraftLobby session for the signed-in account in the Xbox Live session
/// directory (MPSD), the way Minecraft does for a world opened to friends:
///   RTA connection -> PUT session (host as member, lobby properties) -> activity handle -> presence.
/// It reconnects and republishes by itself when RTA drops, and keeps per-joiner nonces up to date.
/// Everything runs on its own thread; the public methods are thread-safe and never block on the network.
class LobbySession {
public:
    LobbySession(ll::io::Logger& logger, auth::XboxAuth& auth, LobbyInfo info);
    ~LobbySession(); // Leaves the session (best effort) and stops.

    void setPlayers(int players);
    void setInfo(LobbyInfo info);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace xbl
} // namespace xbox_live_on_bds
