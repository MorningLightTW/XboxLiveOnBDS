#pragma once

#include <functional>
#include <optional>

#include "xbox_live_on_bds/LinkMonitor.h"

class NetworkIdentifier;
namespace ll::io {
class Logger;
}

namespace xbox_live_on_bds {

/// "/xbl ping": a player's own connection to the server: how it is carried (an Xbox Live friend join, direct or
/// through Microsoft's relay, or another kind), ping, jitter and packet loss, in the player's game language. No
/// addresses or ids. "/xbl ping <player>" shows others' and needs operator rights (anyone may name themselves).
struct PingCommandSources {
    std::function<std::optional<LinkMonitor::Stats>(Player const&)> stats;
    std::function<bool(NetworkIdentifier const&)>                   joinedThroughXboxLive;
};

/// Registers the command; call from enable(), after the mod's lang files are loaded. Also gives each player the
/// command's description in their game language: the command list is plain text, so it is rewritten per player.
void registerPingCommand(ll::io::Logger& logger, PingCommandSources sources);

} // namespace xbox_live_on_bds
