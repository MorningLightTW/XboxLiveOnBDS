#include "ServerLobbyInfo.h"

#include <variant>

#include <fmt/format.h>

#include "ll/api/Versions.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/deps/nether_net/NetworkID.h"
#include "mc/network/TransportLayer.h"
#include "mc/server/PropertiesSettings.h"
#include "mc/world/level/GameType.h"

namespace xbox_live_on_bds {

std::optional<xbl::LobbyInfo> readServerLobbyInfo() {
    auto properties = ll::service::getPropertiesSettings();
    if (!properties) return std::nullopt;

    xbl::LobbyInfo info;
    info.worldName  = properties->mLevelName;
    info.maxPlayers = properties->mMaxPlayers;
    info.localPort  = properties->mServerPort;
    // "1.26.60", as the game reports itself; to_string() would append the build hash ("+41512d4").
    auto version  = ll::getGameVersion();
    info.version  = fmt::format("{}.{}.{}", version.major, version.minor, version.patch);
    info.protocol = ll::getNetworkProtocolVersion();
    switch (properties->mGameType) {
    case GameType::Creative:
        info.gameMode = "Creative";
        break;
    case GameType::Adventure:
        info.gameMode = "Adventure";
        break;
    default:
        info.gameMode = "Survival";
        break;
    }
    NetherNet::NetworkID const& id = properties->mNetherNetId;
    if (auto const* p2p = std::get_if<NetherNet::P2P::NetworkID>(&id)) info.netherNetId = p2p->mValue;
    return info;
}

std::vector<std::string> findJoinBlockers() {
    std::vector<std::string> blockers;
    auto                     properties = ll::service::getPropertiesSettings();
    if (!properties) return blockers;
    if (properties->mTransportLayerType != TransportLayer::NetherWebSockets) {
        blockers.emplace_back(
            "server.properties sets a transport other than NetherNet; friends' games can only join over NetherNet "
            "(remove the \"transport\" line or set it to \"nethernet\")"
        );
    }
    NetherNet::NetworkID const& id  = properties->mNetherNetId;
    auto const*                 p2p = std::get_if<NetherNet::P2P::NetworkID>(&id);
    if (!p2p || p2p->mValue == 0) {
        blockers.emplace_back("the server has no NetherNet id, so friends can see it but not join");
    }
    if (properties->mUseAllowList) {
        blockers.emplace_back(
            "allow-list is on: only players on the allow list can join (add friends with \"allowlist add <name>\", "
            "or set allow-list=false)"
        );
    }
    return blockers;
}

} // namespace xbox_live_on_bds
