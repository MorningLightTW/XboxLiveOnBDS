#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ll::io {
class Logger;
}
class NetworkIdentifier;

namespace xbox_live_on_bds::bds {

/// Puts friends' signals straight into BDS's NetherNet transport, as if they came over its own WebSocket
/// signaling channel, and hands the transport's replies to those peers (CONNECTRESPONSE, CANDIDATEADD,
/// CONNECTERROR) back to the caller. This is how a world hosted by the game is joined: trickle ICE, both
/// sides checking at once, which BDS's HTTP signaling (one complete answer, no trickle) cannot do across NAT.
namespace transport_bridge {

/// Installs the hooks. Call from load(), before the server builds its network.
void install(ll::io::Logger& logger);

/// Whether the transport has been seen yet (it is, once it handles its first signal after start).
[[nodiscard]] bool ready();

/// Gets the transport seen before any friend joins: BDS's HTTP signaling on `port` hands a request to the
/// transport, which turns down an empty offer at once (CONNECTERROR, no peer connection). Blocks for that
/// request; call it off the game's threads. Returns ready().
bool prime(std::uint16_t port);

/// Delivers "<TYPE> <connection id> <payload>" from the peer `from`. False if the transport is not known yet.
bool deliver(std::uint64_t from, std::string_view signal);

/// Whether a connected player came in through deliver(): a friend joining from the Xbox Live Friends tab, as
/// opposed to NetherNet over the LAN or by address, or RakNet.
[[nodiscard]] bool joinedThroughXboxLive(NetworkIdentifier const& player);

/// A player got into the game: an Xbox Live mark of theirs now lasts until onPlayerLeft(). Server thread.
void onPlayerJoined(NetworkIdentifier const& player);

/// A player left: drops their Xbox Live mark, so the same client joining again some other way is not taken for an
/// Xbox Live join. Server thread.
void onPlayerLeft(NetworkIdentifier const& player);

/// Receives the transport's signals to peers that deliver() introduced, as "<TYPE> <connection id> <payload>".
/// Called on NetherNet's own thread. Pass an empty function to stop.
void setSink(std::function<void(std::uint64_t to, std::string signal)> sink);

struct RelayServer {
    std::vector<std::string> urls; // "turn:host:port", "stun:host:port"
    std::string              username;
    std::string              password;
};

/// The TURN relays from the signaling service. They become the only ICE servers of each new peer connection for a
/// join that deliver() brought in, as for a world the game hosts: with them, the server and a friend behind NAT can
/// still reach each other. Other joins keep the server's own ICE servers. Replaces the previous list.
void setRelayServers(std::vector<RelayServer> servers);

} // namespace transport_bridge
} // namespace xbox_live_on_bds::bds
