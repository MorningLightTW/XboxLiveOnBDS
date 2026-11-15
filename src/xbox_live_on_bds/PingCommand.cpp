#include "xbox_live_on_bds/PingCommand.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/certificates/WebToken.h"
#include "mc/network/ConnectionRequest.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/NetworkSystem.h"
#include "mc/network/Packet.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandSelector.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"

namespace xbox_live_on_bds {

// The command's parameters are read by reflection, which needs a type with linkage (not in the anonymous namespace).
struct PingTargetParam {
    CommandSelector<Player> player;
};

namespace {

using namespace ll::i18n_literals;

constexpr std::string_view CommandName = "xbl";
constexpr auto             Description = "XboxLiveOnBDS: your connection to this server"_trl;

ll::io::Logger* logger = nullptr;
std::string     registeredDescription; // As registered, in the server's language; set before the hook goes in.

/// The game reads command output as a translation format, where '%' starts an argument: "0.0%" shows as "0.0".
std::string escapePercent(std::string_view text) {
    std::string out;
    for (char c : text) {
        out += c;
        if (c == '%') out += '%';
    }
    return out;
}

/// A string as the packet writes it: its length as an unsigned varint, then its bytes.
void appendVarString(std::string& out, std::string_view text) {
    auto size = text.size();
    do {
        auto byte   = static_cast<unsigned char>(size & 0x7f);
        size      >>= 7;
        out        += static_cast<char>(size ? byte | 0x80 : byte);
    } while (size);
    out += text;
}

/// The game language a connected client reported at login ("zh_TW", "en_US"...); empty if unknown. Unlike
/// Player::getLocaleCode, it works before the player is in the level, when the command list goes out.
std::string localeOf(NetworkIdentifier const& id) {
    auto handler = ll::service::getServerNetworkHandler();
    if (!handler) return {};
    auto const& clients = handler->mClients.get();
    auto        client  = clients.find(id);
    if (client == clients.end() || !client->second) return {};
    auto const& request = client->second->mPrimaryRequest.get();
    if (!request) return {};
    auto const& token = request->mRawToken.get();
    if (!token) return {};
    auto const& code = std::as_const(token->mDataInfo)["LanguageCode"];
    if (!code.isString()) return {};
    auto locale = code.asString("");
    return locale.size() <= 16 ? locale : std::string{}; // Sent by the client: only a short code is a locale.
}

/// The serialized command list with "/xbl"'s description in the client's language, or nullopt to send it as is.
std::optional<std::string> localizeCommandList(NetworkIdentifier const& id, std::string const& data) {
    auto locale     = localeOf(id);
    auto translated = Description(locale);
    if (translated == registeredDescription) return std::nullopt;
    std::string from;
    appendVarString(from, CommandName);
    appendVarString(from, registeredDescription);
    auto at = data.find(from);
    if (at == std::string::npos) return std::nullopt;
    std::string out;
    out.reserve(data.size() + translated.size());
    out.append(data, 0, at);
    appendVarString(out, CommandName);
    appendVarString(out, translated);
    out.append(data, at + from.size());
    if (logger) logger->debug("Command list: /{} described in {}", CommandName, locale);
    return out;
}

// Every packet to a client passes here once per recipient, already serialized: the command list is the only one
// touched. Command descriptions reach the client as plain text, so each player's copy is written in their language.
LL_TYPE_INSTANCE_HOOK(
    LocalizeCommandListHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::_sendInternal,
    void,
    NetworkIdentifier const& id,
    Packet const&            packet,
    std::string const&       data
) {
    if (packet.getId() == MinecraftPacketIds::AvailableCommands) {
        if (auto localized = localizeCommandList(id, data)) return origin(id, packet, *localized);
    }
    origin(id, packet, data);
}

std::string routeText(std::string_view locale, PingCommandSources const& sources, Player const& player, bool relay) {
    auto const& id = player.getNetworkIdentifier();
    if (id.mType != NetworkIdentifier::Type::NetherNet) return "RakNet (by address)"_trl(locale);
    if (sources.joinedThroughXboxLive && sources.joinedThroughXboxLive(id)) {
        return relay ? "Xbox Live friend join, through Microsoft's relay"_trl(locale)
                     : "Xbox Live friend join, direct (P2P)"_trl(locale);
    }
    return relay ? "NetherNet (LAN or by address), through a relay"_trl(locale)
                 : "NetherNet (LAN or by address), direct"_trl(locale);
}

void report(
    PingCommandSources const& sources,
    std::string_view          locale,
    CommandOutput&            output,
    Player const&             player,
    bool                      self
) {
    auto status = player.getNetworkStatus();
    if (!status) {
        output.error(escapePercent("No connection data for {} yet."_trl(locale, player.getRealName())));
        return;
    }
    std::string text  = self ? "Your connection:"_trl(locale) : "{}'s connection:"_trl(locale, player.getRealName());
    text             += "\n" + "Route: {}"_trl(locale, routeText(locale, sources, player, status->mUsingRelays));
    text += "\n"
          + "Ping: {} ms now, {} ms average"_trl(locale, status->mCurrentPing->count(), status->mAveragePing->count());
    auto stats = sources.stats ? sources.stats(player) : std::nullopt;
    text +=
        "\n"
        + (stats && stats->jitter ? "Jitter: {:.0f} ms"_trl(locale, *stats->jitter) : "Jitter: measuring"_trl(locale));
    text += "\n"
          + "Packet loss: {:.1f}% now, {:.1f}% average"_trl(
                locale,
                status->mCurrentPacketLoss * 100.0f,
                status->mAveragePacketLoss * 100.0f
          );
    if (stats) {
        text +=
            "\n" + "Connected {:.0f} min, {}% of it through a relay"_trl(locale, stats->minutes, stats->relayPercent);
    }
    output.success(escapePercent(text));
}

} // namespace

void registerPingCommand(ll::io::Logger& log, PingCommandSources sources) {
    logger                = &log;
    registeredDescription = Description(ll::i18n::getDefaultLocaleCode());
    auto  shared          = std::make_shared<PingCommandSources>(std::move(sources));
    auto& command         = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
        std::string{CommandName},
        registeredDescription,
        CommandPermissionLevel::Any
    );
    static ll::memory::HookRegistrar<LocalizeCommandListHook> registrar;

    command.overload().text("ping").execute([shared](CommandOrigin const& origin, CommandOutput& output) {
        auto  locale = origin.getLocaleCode();
        auto* entity = origin.getEntity();
        if (!entity || !entity->isPlayer()) {
            output.error(escapePercent("Name a player when running this outside the game."_trl(locale)));
            return;
        }
        report(*shared, locale, output, *static_cast<Player*>(entity), true);
    });

    command.overload<PingTargetParam>().text("ping").required("player").execute(
        [shared](CommandOrigin const& origin, CommandOutput& output, PingTargetParam const& param) {
            auto  locale  = origin.getLocaleCode();
            auto  results = param.player.results(origin);
            auto* self    = origin.getEntity();
            if (results.empty()) {
                output.error(escapePercent("No player matched."_trl(locale)));
                return;
            }
            // Anyone may name themselves; looking at others takes operator rights (the console has them).
            bool others = false;
            for (Player* player : results) others = others || player != self;
            if (others && origin.getPermissionsLevel() < CommandPermissionLevel::GameDirectors) {
                output.error(escapePercent("Only operators can view other players' connections."_trl(locale)));
                return;
            }
            for (Player* player : results) {
                if (player) report(*shared, locale, output, *player, player == self);
            }
        }
    );
}

} // namespace xbox_live_on_bds
