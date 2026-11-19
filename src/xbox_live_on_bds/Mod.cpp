#include "xbox_live_on_bds/Mod.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "ll/api/Config.h"

#include "mc/server/ServerPlayer.h"

#include "xbox_live_on_bds/Config.h"
#include "xbox_live_on_bds/LinkMonitor.h"
#include "xbox_live_on_bds/PingCommand.h"
#include "xbox_live_on_bds/auth/XboxAuth.h"
#include "xbox_live_on_bds/bds/ServerLobbyInfo.h"
#include "xbox_live_on_bds/bds/SignalingTrace.h"
#include "xbox_live_on_bds/bds/TransportBridge.h"
#include "xbox_live_on_bds/net/UdpPorts.h"
#include "xbox_live_on_bds/util/Redact.h"
#include "xbox_live_on_bds/xbl/LobbySession.h"

namespace xbox_live_on_bds {

Mod& Mod::getInstance() {
    static Mod instance;
    return instance;
}

Mod::Mod() : mSelf(*ll::mod::NativeMod::current()) {}

Mod::~Mod() = default;

void Mod::loadConfig() {
    auto&  logger = getSelf().getLogger();
    auto   path   = getSelf().getConfigDir() / "config.json";
    Config config;
    try {
        // Creates the file with the defaults, or adds the fields an older file lacks.
        if (!ll::config::loadConfig(config, path)) ll::config::saveConfig(config, path);
    } catch (std::exception const& e) {
        // Leave a broken file as it is for its owner to fix, rather than overwrite it.
        logger.warn("Could not read {} ({}); using the defaults", path.string(), e.what());
        config = {};
    }
    mVisibility = config.visibility == "friends" ? xbl::Visibility::Friends : xbl::Visibility::FriendsOfFriends;
    if (config.visibility != "friends" && config.visibility != "friendsOfFriends") {
        logger.warn(
            "config.json: visibility \"{}\" is not \"friends\" or \"friendsOfFriends\"; using \"friendsOfFriends\"",
            config.visibility
        );
    }
}

bool Mod::load() {
    getSelf().getLogger().info("Loading XboxLiveOnBDS v{}", XBOX_LIVE_ON_BDS_VERSION);
    loadConfig();
    // Without them everything is in English, so a missing lang folder only warns.
    if (auto loaded = ll::i18n::getInstance().load(getSelf().getLangDir()); !loaded) {
        getSelf().getLogger().warn("Could not load {}; using English", getSelf().getLangDir().string());
        loaded.error().log(getSelf().getLogger());
    }
    // Holds the Microsoft account refresh token and the device key: keep the data directory private.
    mAuth = std::make_unique<auth::XboxAuth>(getSelf().getLogger(), getSelf().getDataDir() / "auth.json");
    bds::transport_bridge::install(getSelf().getLogger()); // Before the server builds its network.
    // Also before it binds its UDP ports: afterwards they are in use by the server itself.
    if (auto warning = net::checkServerUdpPorts("server.properties")) {
        getSelf().getLogger().warn("Friends' joins: {}", *warning);
    }
    // Debugging switches, empty files in the data directory:
    // - "debug.flag" turns this mod's log up to debug (why a connection dropped, reconnects, held joins, per-player
    //   link quality), logs every NetherNet signal and writes joins relayed over HTTP (offer, answer, ids) to
    //   data/debug/. Addresses, ids, names and tokens in the log and in those files are replaced by aliases, so
    //   they can go into a public issue.
    // - "debug-raw.flag" does the same without the replacing, and implies debug. The log and the files then hold
    //   players' addresses and identity tokens: never share them, and delete the files when done.
    auto dataDir = getSelf().getDataDir();
    bool raw     = std::filesystem::exists(dataDir / "debug-raw.flag");
    if (raw || std::filesystem::exists(dataDir / "debug.flag")) {
        mDebug = true;
        util::Redactor::instance().setEnabled(!raw);
        getSelf().getLogger().setLevel(ll::io::LogLevel::Debug);
        bds::enableSignalingTrace(getSelf().getLogger(), dataDir / "debug");
        getSelf().getLogger().warn(
            "Debug mode: debug log, NetherNet signaling trace, join dumps in the data/debug folder of this mod"
        );
        if (raw) {
            getSelf().getLogger().warn(
                "RAW debug mode (debug-raw.flag): the log and data/debug hold IP addresses, ids and tokens. Do not "
                "share them"
            );
        }
    }
    return true;
}

void Mod::startSession() {
    if (mSession) return;
    auto info = bds::readServerLobbyInfo();
    if (!info) {
        getSelf().getLogger().error("Server properties are not loaded; cannot start the Xbox Live session");
        return;
    }
    for (auto const& blocker : bds::findJoinBlockers()) getSelf().getLogger().warn("Friends' joins: {}", blocker);
    info->players    = static_cast<int>(mOnline.size());
    info->visibility = mVisibility;
    if (mDebug) info->debugDir = getSelf().getDataDir() / "debug";
    info->inject = [](std::uint64_t from, std::string_view signal) {
        return bds::transport_bridge::deliver(from, signal);
    };
    info->setInjectSink = [](auto sink) { bds::transport_bridge::setSink(std::move(sink)); };
    info->onTurnServers = [](std::vector<xbl::Signaling::TurnServer> servers) {
        std::vector<bds::transport_bridge::RelayServer> relays;
        for (auto& s : servers) relays.push_back({std::move(s.urls), std::move(s.username), std::move(s.password)});
        bds::transport_bridge::setRelayServers(std::move(relays));
    };
    info->prepareTransport = [port = info->localPort] { return bds::transport_bridge::prime(port); };
    mSession               = std::make_unique<xbl::LobbySession>(getSelf().getLogger(), *mAuth, std::move(*info));
}

void Mod::stopSession() {
    mSession.reset(); // Leaves the Xbox Live session; may wait for one in-flight request.
}

bool Mod::enable() {
    auto& bus = ll::event::EventBus::getInstance();
    mLinks    = std::make_unique<LinkMonitor>(getSelf().getLogger());
    mLinks->start();
    registerPingCommand(
        getSelf().getLogger(),
        {
            .stats = [this](Player const& player) -> std::optional<LinkMonitor::Stats> {
                return mLinks ? mLinks->stats(player) : std::nullopt;
            },
            .joinedThroughXboxLive =
                [](NetworkIdentifier const& id) { return bds::transport_bridge::joinedThroughXboxLive(id); },
        }
    );

    mListeners.push_back(bus.emplaceListener<ll::event::ServerStartedEvent>([this](auto&) { startSession(); }));
    mListeners.push_back(bus.emplaceListener<ll::event::ServerStoppingEvent>([this](auto&) {
        mLinks->onServerStopping();
        stopSession();
    }));
    mListeners.push_back(bus.emplaceListener<ll::event::PlayerJoinEvent>([this](ll::event::PlayerJoinEvent& event) {
        mOnline.insert(event.self().getUuid().asString());
        if (mSession) mSession->setPlayers(static_cast<int>(mOnline.size()));
        mLinks->onJoin(event.self());
        bds::transport_bridge::onPlayerJoined(event.self().getNetworkIdentifier());
    }));
    mListeners.push_back(
        bus.emplaceListener<ll::event::PlayerDisconnectEvent>([this](ll::event::PlayerDisconnectEvent& event) {
            mLinks->onLeave(event.self());
            bds::transport_bridge::onPlayerLeft(event.self().getNetworkIdentifier());
            mOnline.erase(event.self().getUuid().asString());
            if (mSession) mSession->setPlayers(static_cast<int>(mOnline.size()));
        })
    );

    // Enabled after the server started (e.g. reloaded): start right away.
    if (ll::service::getLevel()) startSession();
    return true;
}

bool Mod::disable() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : mListeners) bus.removeListener(listener);
    mListeners.clear();
    mLinks.reset();
    stopSession();
    return true;
}

} // namespace xbox_live_on_bds

LL_REGISTER_MOD(xbox_live_on_bds::Mod, xbox_live_on_bds::Mod::getInstance());
