#pragma once

#include <optional>
#include <string>
#include <vector>

#include "xbox_live_on_bds/xbl/LobbySession.h"

namespace xbox_live_on_bds {

/// The lobby as BDS describes itself: server.properties (server-name, level-name, max-players, gamemode),
/// the game version and network protocol, and the server's NetherNet id. nullopt before the server has
/// loaded its properties.
std::optional<xbl::LobbyInfo> readServerLobbyInfo();

/// server.properties settings that keep friends from joining through the Friends tab, each as a sentence
/// saying what to change. Empty when all is well, or before the server has loaded its properties.
std::vector<std::string> findJoinBlockers();

} // namespace xbox_live_on_bds
