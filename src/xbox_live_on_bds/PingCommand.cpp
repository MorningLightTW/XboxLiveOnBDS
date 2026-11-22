#include "xbox_live_on_bds/PingCommand.h"

#include <algorithm>
#include <cctype>
#include <exception>
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

#include "mc/common/SubClientId.h"
#include "mc/deps/certificates/WebToken.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/MCRESULT.h"
#include "mc/network/ConnectionRequest.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/NetworkSystem.h"
#include "mc/network/Packet.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/packet/AvailableCommandsPacket.h"
#include "mc/server/ServerPlayer.h"
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOriginType.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandOutputParameter.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandRegistry.h"
#include "mc/server/commands/CommandSelector.h"
#include "mc/server/commands/MinecraftCommands.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"

namespace xbox_live_on_bds {

// The command's parameters are read by reflection, which needs a type with linkage (not in the anonymous namespace).
struct PingTargetParam {
    CommandSelector<Player> player;
};

namespace {

using namespace ll::i18n_literals;

constexpr std::string_view CommandName       = "xbl";
constexpr char             TargetParamName[] = "player"; // The member of PingTargetParam.
constexpr std::string_view LoginText         = "login";  // "xbl login": the server console only.
constexpr auto             Description       = "XboxLiveOnBDS: your connection to this server"_trl;

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

bool isTargetOverload(AvailableCommandsPacketPayload::OverloadData const& overload) {
    auto const& params = overload.params.get();
    return std::ranges::any_of(params, [](auto const& param) { return param.name.get() == TargetParamName; });
}

/// "xbl login": for the server console only, so no player sees it in the command list.
bool isLoginOverload(AvailableCommandsPacketPayload::OverloadData const& overload) {
    auto const& params = overload.params.get();
    return std::ranges::any_of(params, [](auto const& param) { return param.name.get() == LoginText; });
}

AvailableCommandsPacketPayload::CommandData* findCommand(AvailableCommandsPacketPayload& payload) {
    for (auto& command : payload.mCommands.get()) {
        if (command.name.get() == CommandName) return &command;
    }
    return nullptr;
}

/// Whether the player may name other players: the same rule as the check inside the command.
bool canTargetOthers(Player const& player) {
    return player.getCommandPermissionLevel() >= CommandPermissionLevel::GameDirectors;
}

/// The serialized command list without "/xbl login", and without "/xbl ping <player>" for a player who may not use
/// it; nullopt to send it as is. BDS builds one list for everyone, and the client hides whole commands only, so the
/// overloads are cut out for each player.
std::optional<std::string> hideOverloads(NetworkIdentifier const& id, Packet const& packet, std::string const& data) {
    auto handler = ll::service::getServerNetworkHandler();
    if (!handler) return std::nullopt;
    auto* player     = handler->_getServerPlayer(id, SubClientId::PrimaryClient);
    bool  hideTarget = !player || !canTargetOthers(*player);
    auto  hidden     = [&](AvailableCommandsPacketPayload::OverloadData const& overload) {
        return isLoginOverload(overload) || (hideTarget && isTargetOverload(overload));
    };

    auto& original = static_cast<AvailableCommandsPacket const&>(packet);
    bool  present  = false;
    for (auto const& command : original.mCommands.get()) {
        if (command.name.get() != CommandName) continue;
        present = std::ranges::any_of(command.overloads.get(), hidden);
        break;
    }
    if (!present) return std::nullopt;

    auto registry = ll::service::getCommandRegistry();
    if (!registry) return std::nullopt;
    auto fresh   = registry->serializeAvailableCommands();
    auto command = findCommand(fresh);
    if (!command) return std::nullopt;
    std::erase_if(command->overloads.get(), hidden);

    BinaryStream before;
    original.write(before);
    BinaryStream after;
    fresh.write(after);
    auto const& oldBody = before.mBuffer;
    auto const& newBody = after.mBuffer;
    if (oldBody.size() > data.size() || data.compare(data.size() - oldBody.size(), oldBody.size(), oldBody) != 0) {
        static bool logged = false;
        if (!std::exchange(logged, true) && logger) logger->debug("Command list: header not found, sent unchanged");
        return std::nullopt;
    }
    std::string out;
    out.reserve(data.size() - oldBody.size() + newBody.size());
    out.append(data, 0, data.size() - oldBody.size());
    out.append(newBody);
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
    if (packet.getId() != MinecraftPacketIds::AvailableCommands) return origin(id, packet, data);
    try {
        auto        hidden  = hideOverloads(id, packet, data);
        auto const& current = hidden ? *hidden : data;
        if (auto localized = localizeCommandList(id, current)) return origin(id, packet, *localized);
        return origin(id, packet, current);
    } catch (std::exception const& e) {
        if (logger) logger->debug("Command list: {}", e.what());
    } catch (...) {}
    origin(id, packet, data);
}

// A change of operator rights changes who may see "/xbl ping <player>", so the player gets a fresh list (which
// passes the hook above).
LL_TYPE_INSTANCE_HOOK(
    PermissionChangeHook,
    ll::memory::HookPriority::Normal,
    Player,
    &Player::setCommandPermissions,
    void,
    CommandPermissionLevel permissions
) {
    bool before = canTargetOthers(*this);
    origin(permissions);
    try {
        if (isSimulatedPlayer() || before == canTargetOthers(*this)) return;
        if (auto registry = ll::service::getCommandRegistry()) registry->serializeAvailableCommands().sendTo(*this);
    } catch (std::exception const& e) {
        if (logger) logger->debug("Command list refresh: {}", e.what());
    } catch (...) {}
}

// Vanilla's syntax error shows at most this many characters around the bad token.
constexpr size_t SyntaxContextBefore = 10;
constexpr size_t SyntaxContextAfter  = 10;

// The text of the command being executed on this thread, or null. It is set by the hook below.
thread_local std::string const* currentCommandText = nullptr;

struct CommandTextScope {
    std::string const* previous;
    explicit CommandTextScope(std::string const& text) : previous(std::exchange(currentCommandText, &text)) {}
    ~CommandTextScope() { currentCommandText = previous; }
    CommandTextScope(CommandTextScope const&)            = delete;
    CommandTextScope& operator=(CommandTextScope const&) = delete;
};

// The lambda of an overload cannot see the typed text, so every command records it for the duration of its run.
LL_TYPE_INSTANCE_HOOK(
    RecordCommandTextHook,
    ll::memory::HookPriority::Normal,
    MinecraftCommands,
    &MinecraftCommands::executeCommand,
    MCRESULT,
    CommandContext& context,
    bool            suppressOutput
) {
    CommandTextScope scope(context.mCommand);
    return origin(context, suppressOutput);
}

/// Where the word that makes "/xbl ping <arg>" or "/xbl login" fail starts and ends in the typed text: the argument
/// of "ping", or the word after "xbl". An empty range at the end if the text does not look like that.
std::pair<size_t, size_t> badArgumentRange(std::string_view text) {
    size_t at        = 0;
    auto   skipSpace = [&] {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    };
    auto skipWord = [&](std::string_view word) {
        if (text.size() - at < word.size()) return false;
        for (size_t i = 0; i < word.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(text[at + i])) != word[i]) return false;
        }
        at += word.size();
        return at == text.size() || std::isspace(static_cast<unsigned char>(text[at]));
    };
    if (at < text.size() && text[at] == '/') ++at;
    if (skipWord("xbl")) {
        skipSpace();
        if (skipWord("ping")) skipSpace();
        size_t end = at;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        if (end > at) return {at, end};
    }
    return {text.size(), text.size()};
}

/// Vanilla's syntax error, as if the overload did not exist: the client translates it itself.
void syntaxError(CommandOutput& output) {
    std::string_view text = currentCommandText ? std::string_view{*currentCommandText} : std::string_view{};
    auto [start, end]     = badArgumentRange(text);
    auto from             = start > SyntaxContextBefore ? start - SyntaxContextBefore : 0;
    auto to               = std::min(text.size(), end + SyntaxContextAfter);
    output.error(
        "commands.generic.syntax",
        {CommandOutputParameter{std::string{text.substr(from, start - from)}},
         CommandOutputParameter{std::string{text.substr(start, end - start)}},
         CommandOutputParameter{std::string{text.substr(end, to - end)}}}
    );
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
    static ll::memory::HookRegistrar<LocalizeCommandListHook, PermissionChangeHook, RecordCommandTextHook> registrar;

    command.overload().text("ping").execute([shared](CommandOrigin const& origin, CommandOutput& output) {
        auto  locale = origin.getLocaleCode();
        auto* entity = origin.getEntity();
        if (!entity || !entity->isPlayer()) {
            output.error(escapePercent("Name a player when running this outside the game."_trl(locale)));
            return;
        }
        report(*shared, locale, output, *static_cast<Player*>(entity), true);
    });

    // Not in any player's command list. A player who types it anyway gets the vanilla syntax error.
    command.overload().text(LoginText).execute([shared](CommandOrigin const& origin, CommandOutput& output) {
        if (origin.getOriginType() != CommandOriginType::DedicatedServer || !shared->login) {
            syntaxError(output);
            return;
        }
        output.success(escapePercent(shared->login()));
    });

    command.overload<PingTargetParam>()
        .text("ping")
        .required(TargetParamName)
        .execute([shared](CommandOrigin const& origin, CommandOutput& output, PingTargetParam const& param) {
            auto locale = origin.getLocaleCode();
            // Naming a player takes operator rights, whatever the selector matches (the console has them). The command
            // list hides this overload from other players. If one types it anyway, they get the vanilla syntax error,
            // as if the overload did not exist, so they cannot tell that the argument exists.
            if (origin.getPermissionsLevel() < CommandPermissionLevel::GameDirectors) {
                syntaxError(output);
                return;
            }
            auto  results = param.player.results(origin);
            auto* self    = origin.getEntity();
            if (results.empty()) {
                output.error(escapePercent("No player matched."_trl(locale)));
                return;
            }
            for (Player* player : results) {
                if (player) report(*shared, locale, output, *player, player == self);
            }
        });
}

} // namespace xbox_live_on_bds
