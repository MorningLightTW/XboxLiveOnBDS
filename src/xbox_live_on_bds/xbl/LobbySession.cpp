#include "xbox_live_on_bds/xbl/LobbySession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <bcrypt.h>

#include "ll/api/io/Logger.h"

#include "xbox_live_on_bds/auth/XboxAuth.h"
#include "xbox_live_on_bds/net/Http.h"
#include "xbox_live_on_bds/net/HttpLog.h"
#include "xbox_live_on_bds/util/Encoding.h"
#include "xbox_live_on_bds/util/Errors.h"
#include "xbox_live_on_bds/util/Redact.h"
#include "xbox_live_on_bds/xbl/RtaClient.h"
#include "xbox_live_on_bds/xbl/Signaling.h"

namespace xbox_live_on_bds::xbl {

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view Scid         = "4fc10100-5f7a-4470-899b-280835760c07"; // Minecraft
constexpr std::string_view TemplateName = "MinecraftLobby";
constexpr std::string_view HandlesUrl   = "https://sessiondirectory.xboxlive.com/handles";
constexpr std::string_view MpsdContract = "107";

// SupportedConnections[].ConnectionType: WebSocketsWebRTCJsonRpcSignaling (mc/network/ConnectionType.h).
constexpr int ConnectionTypeJsonRpcSignaling = 7;

constexpr auto PresenceInterval = 300s;
constexpr auto ConnectionIdWait = 10s;
constexpr auto FirstRetryDelay  = 5s;
constexpr auto MaxRetryDelay    = 300s;
constexpr auto PutRetryDelay    = 30s;
// The game leaves this server out of the Friends tab when the host account's activity handle points elsewhere,
// e.g. after the account joined a world on another device. Checked this often, and closely after a player leaves.
constexpr auto HandleCheckInterval = 60s;
constexpr auto LeaveProbeStep      = 2s;
constexpr auto LeaveProbeWindow    = 20s;

std::string sessionUrl(std::string_view scid, std::string_view templateName, std::string_view sessionId) {
    return fmt::format(
        "https://sessiondirectory.xboxlive.com/serviceconfigs/{}/sessionTemplates/{}/sessions/{}",
        scid,
        templateName,
        sessionId
    );
}

std::string sessionUrl(std::string_view sessionId) { return sessionUrl(Scid, TemplateName, sessionId); }

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::string randomHex(std::size_t bytes) {
    std::array<std::uint8_t, 32> b{};
    bytes = std::min(bytes, b.size());
    BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string out;
    for (std::size_t i = 0; i < bytes; ++i) out += fmt::format("{:02x}", b[i]);
    return out;
}

/// The Friends tab's player count. A world the game hosts counts its host, so with `IncludeHost` the server counts
/// as one more player, but only while that stays below the limit: one free slot must not show as full. Never below 1,
/// or the Friends tab hides the world.
int memberCount(PlayerCount mode, int players, int maxPlayers) {
    if (mode == PlayerCount::IncludeHost && players + 1 < maxPlayers) ++players;
    return std::max(players, 1);
}

} // namespace

struct LobbySession::Impl {
    ll::io::Logger& logger;
    auth::XboxAuth& auth;

    // Shared with the public setters and the RTA thread; guarded by `mutex`.
    std::mutex                  mutex;
    std::condition_variable_any wake;
    LobbyInfo                   info;
    bool                        infoChanged    = false;
    bool                        membersChanged = false;
    bool                        rtaLost        = false;
    std::string                 rtaWhy; // why the RTA socket ended
    bool                        signalingLost = false;
    std::string                 signalingWhy; // why the signaling socket ended
    bool                        playerLeft     = false;
    bool                        retryRequested = false; // "xbl login": try the sign-in again now
    std::string                 connectionId;

    // Worker thread only.
    std::string                        sessionId      = util::randomUuid();
    std::string                        subscriptionId = util::randomUuid();
    auth::XstsToken                    token;
    std::unique_ptr<RtaClient>         rta;
    std::unique_ptr<Signaling>         signaling;
    std::map<std::string, std::string> nonces; // joining XUID -> nonce
    bool                               published = false;
    bool unsupported = false; // The server's NetherNet is not the one this mod knows; session thread only.

    /// Where the host account's activity handle (its Friends-tab entry) points.
    enum class Handle { Ours, Gone, Elsewhere, Unknown };
    Handle handle = Handle::Gone;

    std::jthread worker;

    Impl(ll::io::Logger& logger, auth::XboxAuth& auth, LobbyInfo initial)
    : logger(logger),
      auth(auth),
      info(std::move(initial)),
      worker([this](std::stop_token stop) { runGuarded(stop); }) {}

    // ---- requests -----------------------------------------------------------------------------------------

    net::Headers headers(std::string_view contract) const {
        return {
            {"Authorization",          token.authorizationHeader()},
            {"x-xbl-contract-version", std::string(contract)      },
            {"Content-Type",           "application/json"         },
            {"Accept-Language",        "en-US"                    }
        };
    }

    json sessionBody(LobbyInfo const& i, std::string const& connection) const {
        json nonceMap = json::object();
        for (auto const& [xuid, nonce] : nonces) nonceMap[xuid] = nonce;
        return {
            {"members",
             {{"me",
               {{"constants", {{"system", {{"xuid", token.xuid}, {"initialize", true}}}}},
                {"properties",
                 {{"system",
                   {{"active", true},
                    {"connection", connection},
                    {"subscription", {{"id", subscriptionId}, {"changeTypes", json::array({"everything"})}}}}}}}}}}},
            {"properties",
             {{"system", {{"joinRestriction", "followed"}, {"readRestriction", "followed"}, {"closed", false}}},
              {"custom",
               {{"BroadcastSetting", static_cast<int>(i.visibility)},
                {"CrossPlayDisabled", false},
                {"Joinability", "joinable_by_friends"},
                {"LanGame", false},
                {"MaxMemberCount", i.maxPlayers},
                {"MemberCount", memberCount(i.playerCount, i.players, i.maxPlayers)},
                {"OnlineCrossPlatformGame", true},
                {"SupportedConnections",
                 json::array(
                     {{{"ConnectionType", ConnectionTypeJsonRpcSignaling},
                       {"HostIpAddress", ""},
                       {"HostPort", 0},
                       {"NetherNetId", i.netherNetId},
                       {"PmsgId", i.pmsgId}}}
                 )},
                {"TitleId", 0},
                {"TransportLayer", 2},
                {"levelId", "level"},
                {"hostName", token.gamertag}, // The host account itself, as when the game hosts a world.
                {"ownerId", token.xuid},
                {"rakNetGUID", ""},
                {"worldName", i.worldName},
                {"worldType", i.gameMode},
                {"protocol", i.protocol},
                {"version", i.version},
                {"isEditorWorld", false},
                {"isHardcore", false},
                {"nonces", nonceMap}}}}                                                                            }
        };
    }

    bool putSession() {
        LobbyInfo   snapshot;
        std::string connection;
        {
            std::lock_guard lock(mutex);
            snapshot   = info;
            connection = connectionId;
        }
        auto res =
            net::request("PUT", sessionUrl(sessionId), headers(MpsdContract), sessionBody(snapshot, connection).dump());
        if (!res.ok()) {
            net::logFailure(logger, "Friends tab: could not show this server", res, "Session publish");
            return false;
        }
        return true;
    }

    bool createHandle() {
        json body = {
            {"version",    1                                                                    },
            {"type",       "activity"                                                           },
            {"sessionRef", {{"scid", Scid}, {"templateName", TemplateName}, {"name", sessionId}}}
        };
        auto res = net::request("POST", HandlesUrl, headers(MpsdContract), body.dump());
        if (!res.ok()) {
            net::logFailure(logger, "Friends tab: could not show this server", res, "Activity handle");
            return false;
        }
        return true;
    }

    void updatePresence() {
        auto res = net::request(
            "POST",
            fmt::format("https://userpresence.xboxlive.com/users/xuid({})/devices/current/titles/current", token.xuid),
            headers("3"),
            R"({"state":"active"})"
        );
        if (!res.ok()) {
            logger.warn("Friends tab: could not update the online status of the host account ({})", res.plainCause());
            logger.debug("Presence update: {}", util::Redactor::instance().text(res.describe()));
        }
    }

    /// Joining players add themselves to the session; each needs a nonce from the host.
    enum class Nonces { Unchanged, Republished, Failed };

    Nonces updateNonces() {
        auto res = net::request("GET", sessionUrl(sessionId), headers(MpsdContract));
        auto j   = json::parse(res.body, nullptr, false);
        if (!res.ok() || !j.is_object()) {
            logger.warn("Friends' joins: could not read the players who join ({}); trying again", res.plainCause());
            logger.debug("Session members: {}", util::Redactor::instance().text(res.describe()));
            return Nonces::Failed;
        }
        std::set<std::string> joiners;
        try {
            for (auto const& [index, member] : j.value("members", json::object()).items()) {
                auto xuid = member.value("/constants/system/xuid"_json_pointer, std::string{});
                if (!xuid.empty() && xuid != token.xuid) joiners.insert(xuid);
            }
        } catch (json::exception const&) {
            logger.warn("Friends' joins: could not read the players who join (unexpected answer); trying again");
            logger.debug("Session members: {}", util::Redactor::instance().text(res.describe()));
            return Nonces::Failed;
        }
        bool changed = std::erase_if(nonces, [&](auto const& entry) { return !joiners.contains(entry.first); }) > 0;
        for (auto const& xuid : joiners) {
            if (!nonces.contains(xuid)) {
                nonces[xuid] = randomHex(8);
                changed      = true;
            }
        }
        if (!changed) return Nonces::Unchanged;
        return putSession() ? Nonces::Republished : Nonces::Failed;
    }

    /// Whether the host account is an active member of another session; empty when that could not be read.
    std::optional<bool> hostIsActiveIn(json const& ref) const {
        auto url = sessionUrl(ref.value("scid", ""), ref.value("templateName", ""), ref.value("name", ""));
        auto res = net::request("GET", url, headers(MpsdContract));
        if (res.error.empty() && (res.status == 403 || res.status == 404)) return false; // gone, or not a member
        auto j = json::parse(res.body, nullptr, false);
        if (!res.ok() || !j.is_object()) return std::nullopt;
        try {
            for (auto const& [index, member] : j.value("members", json::object()).items()) {
                if (member.value("/constants/system/xuid"_json_pointer, std::string{}) != token.xuid) continue;
                return member.value("/properties/system/active"_json_pointer, true);
            }
        } catch (json::exception const&) {
            return std::nullopt;
        }
        return false;
    }

    /// Reads the host account's activity handle the way the game's Friends tab does (handles/query by owner).
    Handle checkHandle() {
        json query = {
            {"type",   "activity"                            },
            {"scid",   Scid                                  },
            {"owners", {{"xuids", json::array({token.xuid})}}}
        };
        auto res = net::request(
            "POST",
            fmt::format("{}/query?include=relatedInfo", HandlesUrl),
            headers(MpsdContract),
            query.dump()
        );
        auto j = json::parse(res.body, nullptr, false);
        if (!res.ok() || !j.is_object()) {
            logger.debug(
                "Could not read the host account's activity handle: {}",
                util::Redactor::instance().text(res.describe())
            );
            return Handle::Unknown;
        }
        try {
            for (auto const& h : j.value("results", json::array())) {
                if (h.value("ownerXuid", std::string{}) != token.xuid) continue;
                auto const& ref = h.at("sessionRef");
                if (equalsIgnoreCase(ref.value("name", ""), sessionId)) return Handle::Ours;
                // A handle left behind by a device that is no longer in that world does not count.
                auto active = hostIsActiveIn(ref);
                if (!active) return Handle::Unknown;
                return *active ? Handle::Elsewhere : Handle::Gone;
            }
        } catch (json::exception const&) {
            logger.debug(
                "Could not read the host account's activity handle: {}",
                util::Redactor::instance().text(res.describe())
            );
            return Handle::Unknown;
        }
        return Handle::Gone;
    }

    void noteElsewhere() {
        if (handle == Handle::Elsewhere) return;
        handle = Handle::Elsewhere;
        logger.warn(
            "Friends tab: the host account is in another world on another device; friends do not see this server until "
            "it "
            "leaves that world"
        );
    }

    /// Puts this server back in the Friends tab when its entry is gone, without taking it from another device.
    /// Returns whether the entry was found gone.
    bool maintainHandle() {
        switch (checkHandle()) {
        case Handle::Unknown:
            return false;
        case Handle::Ours:
            handle = Handle::Ours;
            return false;
        case Handle::Elsewhere:
            noteElsewhere();
            return false;
        case Handle::Gone:
            if (handle == Handle::Elsewhere) {
                logger.info("Friends tab: the host account left the other world; showing this server again");
            } else {
                logger.warn("Friends tab: this server dropped out; showing it again");
            }
            if (putSession() && createHandle()) {
                handle = Handle::Ours;
                updatePresence();
            }
            return true;
        }
        return false;
    }

    void leave() {
        if (!published) return;
        net::request("PUT", sessionUrl(sessionId), headers(MpsdContract), R"({"members":{"me":null}})");
        published = false;
    }

    // ---- connections --------------------------------------------------------------------------------------
    //
    // RTA and signaling are replaced on their own when one drops (the services close long-lived sockets from
    // time to time), the way go-xsapi's RTA client does: the session stays listed meanwhile, and only the RTA
    // connection id it advertises changes. Leaving it and republishing would hide the server each time.

    using Clock = std::chrono::steady_clock;

    void dropSignaling() {
        signaling.reset(); // Closing on purpose: ~Signaling does not report it.
        std::lock_guard lock(mutex);
        signalingLost = false;
        signalingWhy.clear();
    }

    void dropRta() {
        rta.reset(); // Its onClosed has run by now; forget it.
        std::lock_guard lock(mutex);
        rtaLost = false;
        rtaWhy.clear();
        connectionId.clear();
    }

    /// The Minecraft signaling service, which brings friends' joins; with a fresh MCToken once it is due.
    bool connectSignaling(std::stop_token const& stop, bool reconnect, std::string& error) {
        dropSignaling();
        std::string gameVersion;
        {
            std::lock_guard lock(mutex);
            gameVersion = info.version;
        }
        auto mc = auth.getMinecraftSession(gameVersion, stop);
        if (!mc) return false; // The sign-in has logged why; `error` stays empty.
        Signaling::Options options;
        {
            std::lock_guard lock(mutex);
            if (info.pmsgId != mc->playerMessagingId) {
                info.pmsgId = mc->playerMessagingId;
                if (published) infoChanged = true; // The session advertises it.
            }
            options.netherNetId   = info.netherNetId;
            options.relayBaseUrl  = fmt::format("http://127.0.0.1:{}", info.localPort);
            options.debugDir      = info.debugDir;
            options.inject        = info.inject;
            options.setInjectSink = info.setInjectSink;
            options.onTurnServers = info.onTurnServers;
        }
        options.minecraftAuthorization = mc->authorizationHeader;
        options.reconnect              = reconnect;
        options.onClosed               = [this](std::string const& reason) {
            std::lock_guard lock(mutex);
            if (!signalingLost) signalingWhy = reason;
            signalingLost = true;
            wake.notify_all();
        };
        signaling = Signaling::connect(logger, std::move(options), error);
        return signaling != nullptr;
    }

    /// Xbox Live RTA, whose connection id the session member needs (MPSD drops a member whose connection is gone).
    bool connectRta(std::stop_token const& stop, std::string& error) {
        dropRta();
        // Tokens last about 16 h; refresh before they lapse (getXsts returns the cached one otherwise).
        auto xsts = auth.getXsts(auth::XboxLiveRelyingParty, stop);
        if (!xsts) return false; // The sign-in has logged why; `error` stays empty.
        token = *xsts;
        util::Redactor::instance().addPlayer(token.gamertag);
        util::Redactor::instance().addPlayer(token.xuid);
        rta = RtaClient::connect(
            token.authorizationHeader(),
            {.onConnectionId =
                 [this](std::string const& id) {
                     std::lock_guard lock(mutex);
                     if (connectionId.empty()) connectionId = id;
                     wake.notify_all();
                 },
             .onEvent =
                 [this](json const& data) {
                     // MPSD session change (a player joining or leaving the session).
                     if (data.is_object() && data.contains("ncid")) {
                         std::lock_guard lock(mutex);
                         membersChanged = true;
                         wake.notify_all();
                     }
                 },
             .onClosed =
                 [this](std::string const& reason) {
                     std::lock_guard lock(mutex);
                     if (!rtaLost) rtaWhy = reason;
                     rtaLost = true;
                     wake.notify_all();
                 }},
            error
        );
        if (!rta) return false;
        bool gotId;
        {
            std::unique_lock lock(mutex);
            gotId = wake.wait_for(lock, stop, ConnectionIdWait, [this] { return !connectionId.empty() || rtaLost; })
                 && !connectionId.empty();
        }
        if (!gotId) {
            error = "RTA gave no connection id";
            dropRta();
            return false;
        }
        return true;
    }

    /// Puts the session up with the current RTA connection and points the host account's Friends-tab entry at
    /// it. After a lost connection the entry is left to the host account while it plays elsewhere: that is often
    /// another device taking over. The first publish always claims it.
    bool publish(bool claim) {
        if (!putSession()) return false;
        if (!claim && checkHandle() == Handle::Elsewhere) {
            noteElsewhere();
        } else {
            if (!createHandle()) return false;
            handle = Handle::Ours;
        }
        published = true;
        updatePresence();
        return true;
    }

    /// A connection that dropped, tried again with a growing delay until it is back.
    struct Reconnect {
        std::optional<Clock::time_point> at;
        std::chrono::seconds             delay         = FirstRetryDelay;
        int                              failures      = 0;
        bool                             awaitingLogin = false; // Not scheduled: it waits for "xbl login".
    };

    template <class Attempt>
    void reconnect(Reconnect& state, std::string_view service, Attempt&& attempt) {
        std::string error;
        if (attempt(error)) {
            if (state.failures > 0) logger.info("{}: reconnected", service);
            else logger.debug("Reconnected to {}", service);
            state = {};
            return;
        }
        ++state.failures;
        if (!error.empty()) logger.debug("Reconnect to {}: {}", service, util::Redactor::instance().text(error));
        if (auth.awaitingLogin()) {
            // The sign-in has told the owner to type "xbl login"; until then, retrying only fills the log.
            state.at.reset();
            state.awaitingLogin = true;
            return;
        }
        logger.warn("{}: could not reconnect; trying again in {} s", service, state.delay.count());
        state.at    = Clock::now() + state.delay;
        state.delay = std::min<std::chrono::seconds>(state.delay * 2, MaxRetryDelay);
    }

    // ---- lifecycle ----------------------------------------------------------------------------------------

    /// Signaling first, as the game does: a lobby nobody can signal to is not worth listing.
    bool connectAndPublish(std::stop_token const& stop) {
        // Before the server shows up anywhere, so the first friend's join can go straight into BDS's transport.
        std::function<bool()> prepareTransport;
        {
            std::lock_guard lock(mutex);
            prepareTransport = info.prepareTransport;
        }
        if (prepareTransport && !prepareTransport()) {
            unsupported = true; // The transport reported why; nothing is connected or published.
            return false;
        }
        std::string error;
        if (!connectSignaling(stop, false, error)) {
            if (!stop.stop_requested() && !error.empty()) {
                logger.debug("Signaling connect: {}", util::Redactor::instance().text(error));
                logger.warn("Friends' joins: could not connect to the Minecraft service");
            }
            return false;
        }
        if (!connectRta(stop, error)) {
            if (!stop.stop_requested() && !error.empty()) {
                logger.debug("RTA connect: {}", util::Redactor::instance().text(error));
                logger.warn("Xbox Live: could not connect");
            }
            return false;
        }
        if (!publish(true)) return false;
        LobbyInfo snapshot;
        {
            std::lock_guard lock(mutex);
            snapshot    = info;
            infoChanged = false;
        }
        logger.info(
            "Xbox Live session is up: {} of {} can see \"{}\" in the Friends tab",
            snapshot.visibility == Visibility::Friends ? "friends" : "friends and friends of friends",
            token.gamertag,
            snapshot.worldName
        );
        // Not at info level: a console log gets pasted when asking for help, and joins are signaled to this id.
        logger.debug("The session advertises NetherNet id {}", util::Redactor::instance().id(snapshot.netherNetId));
        return true;
    }

    void run(std::stop_token stop) {
        auto retryDelay = std::chrono::seconds(FirstRetryDelay);
        while (!stop.stop_requested() && !connectAndPublish(stop)) {
            dropRta();
            dropSignaling();
            if (unsupported || stop.stop_requested()) break;
            if (auth.awaitingLogin()) {
                // The sign-in has told the owner to type "xbl login": wait for it, without a retry every few minutes.
                std::unique_lock lock(mutex);
                wake.wait(lock, stop, [this] { return retryRequested; });
                retryRequested = false;
                retryDelay     = FirstRetryDelay;
                continue;
            }
            logger.warn("Xbox Live: trying again in {} s", retryDelay.count());
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, retryDelay, [this] { return retryRequested; });
            retryRequested = false;
            retryDelay     = std::min<std::chrono::seconds>(retryDelay * 2, MaxRetryDelay);
        }
        if (!unsupported && !stop.stop_requested()) serve(stop);
        dropSignaling(); // No more joins.
        leave();
        dropRta();
    }

    /// An exception must not leave the worker thread: that would end the whole server.
    void runGuarded(std::stop_token stop) {
        try {
            run(stop);
            return;
        } catch (std::exception const& e) {
            logger.error(
                "Friends tab: the Xbox Live session stopped after an unexpected error; restart the server to show it "
                "in the "
                "Friends tab again"
            );
            logger.debug("Session error: {}", util::Redactor::instance().text(util::describeException(e)));
        } catch (...) {
            logger.error(
                "Friends tab: the Xbox Live session stopped after an unexpected error; restart the server to show it "
                "in the "
                "Friends tab again"
            );
        }
        try {
            dropSignaling();
            leave();
            dropRta();
        } catch (...) {}
    }

    /// Keeps the published session current until stopped.
    void serve(std::stop_token const& stop) {
        auto nextPresence    = Clock::now() + PresenceInterval;
        auto nextHandleCheck = Clock::now() + HandleCheckInterval;
        // After a player leaves, the entry is checked closely, so a gap there shows up in the log.
        std::optional<Clock::time_point> probeUntil;
        bool                             probeSawGone = false;
        // A failed publish (player count, nonces) is tried again rather than left stale until the next change.
        std::optional<Clock::time_point> retryPutAt;
        bool                             membersRetry = false; // the retry covers the nonces
        Reconnect                        rtaReconnect, signalingReconnect;
        while (!stop.stop_requested()) {
            bool        doInfo = false, doMembers = false, left = false, retry = false;
            bool        rtaDropped = false, signalingDropped = false;
            std::string rtaReason, signalingReason;
            {
                // Session upkeep needs RTA's connection id; while RTA is down only the reconnects are due.
                auto until = Clock::now() + 1h;
                if (rta) {
                    until = std::min({until, nextPresence, nextHandleCheck});
                    if (retryPutAt) until = std::min(until, *retryPutAt);
                }
                if (rtaReconnect.at) until = std::min(until, *rtaReconnect.at);
                if (signalingReconnect.at) until = std::min(until, *signalingReconnect.at);
                std::unique_lock lock(mutex);
                wake.wait_until(lock, stop, until, [this] {
                    return rtaLost || signalingLost || retryRequested
                        || (rta && (infoChanged || membersChanged || playerLeft));
                });
                retry            = std::exchange(retryRequested, false);
                rtaDropped       = std::exchange(rtaLost, false);
                rtaReason        = std::exchange(rtaWhy, {});
                signalingDropped = std::exchange(signalingLost, false);
                signalingReason  = std::exchange(signalingWhy, {});
                if (rta && !rtaDropped) {
                    doInfo    = std::exchange(infoChanged, false);
                    doMembers = std::exchange(membersChanged, false);
                    left      = std::exchange(playerLeft, false);
                }
            }
            if (stop.stop_requested()) break;

            if (signalingDropped) {
                logger.debug(
                    "The Minecraft signaling connection ended ({}); reconnecting",
                    util::Redactor::instance().text(signalingReason)
                );
                dropSignaling();
                if (!signalingReconnect.at) signalingReconnect.at = Clock::now();
            }
            if (rtaDropped) {
                logger.debug(
                    "The Xbox Live RTA connection ended ({}); reconnecting",
                    util::Redactor::instance().text(rtaReason)
                );
                dropRta();
                if (!rtaReconnect.at) rtaReconnect.at = Clock::now();
            }
            if (retry) {
                for (auto* state : {&rtaReconnect, &signalingReconnect}) {
                    if (state->awaitingLogin) *state = {.at = Clock::now()};
                }
            }
            if (signalingReconnect.at && Clock::now() >= *signalingReconnect.at) {
                reconnect(signalingReconnect, "Friends' joins", [&](std::string& error) {
                    return connectSignaling(stop, true, error);
                });
            }
            if (rtaReconnect.at && Clock::now() >= *rtaReconnect.at) {
                // The session is put again in full, with the new connection id. A connected RTA whose publish
                // failed is kept for the next try.
                reconnect(rtaReconnect, "Xbox Live", [&](std::string& error) {
                    if (!rta && !connectRta(stop, error)) return false;
                    if (publish(false)) return true;
                    return false;
                });
                if (!rtaReconnect.at) {
                    retryPutAt.reset();
                    membersRetry    = false;
                    nextPresence    = Clock::now() + PresenceInterval;
                    nextHandleCheck = Clock::now() + HandleCheckInterval;
                    std::lock_guard lock(mutex);
                    membersChanged = true; // Players may have joined or left while it was down.
                }
                continue;
            }
            if (!rta) continue;

            if (retryPutAt && Clock::now() >= *retryPutAt) {
                doInfo = true;
                if (membersRetry) doMembers = true;
            }
            if (auto xsts = auth.getXsts(auth::XboxLiveRelyingParty, stop)) {
                token = *xsts;
                util::Redactor::instance().addPlayer(token.gamertag);
                util::Redactor::instance().addPlayer(token.xuid);
            }
            auto nonces  = doMembers ? updateNonces() : Nonces::Unchanged;
            membersRetry = nonces == Nonces::Failed;
            bool ok      = nonces != Nonces::Failed;
            if (ok && doInfo && nonces != Nonces::Republished) ok = putSession();
            if (!ok) retryPutAt = Clock::now() + PutRetryDelay;
            else if (doInfo || doMembers) retryPutAt.reset();
            if (Clock::now() >= nextPresence) {
                updatePresence();
                nextPresence = Clock::now() + PresenceInterval;
            }
            if (left) {
                probeUntil      = Clock::now() + LeaveProbeWindow;
                probeSawGone    = false;
                nextHandleCheck = Clock::now() + LeaveProbeStep;
            }
            if (Clock::now() >= nextHandleCheck) {
                if (maintainHandle() && probeUntil) probeSawGone = true;
                if (probeUntil && Clock::now() >= *probeUntil) {
                    if (!probeSawGone) {
                        logger.debug(
                            "The Friends-tab entry stayed up for {} s after a player left",
                            LeaveProbeWindow.count()
                        );
                    }
                    probeUntil.reset();
                }
                nextHandleCheck = Clock::now() + (probeUntil ? LeaveProbeStep : HandleCheckInterval);
            }
        }
    }
};

LobbySession::LobbySession(ll::io::Logger& logger, auth::XboxAuth& auth, LobbyInfo info)
: mImpl(std::make_unique<Impl>(logger, auth, std::move(info))) {}

LobbySession::~LobbySession() {
    mImpl->worker.request_stop();
    if (mImpl->worker.joinable()) mImpl->worker.join();
}

void LobbySession::setPlayers(int players) {
    std::lock_guard lock(mImpl->mutex);
    if (mImpl->info.players == players) return;
    if (players < mImpl->info.players) mImpl->playerLeft = true;
    mImpl->info.players = players;
    mImpl->infoChanged  = true;
    mImpl->wake.notify_all();
}

void LobbySession::retryNow() {
    std::lock_guard lock(mImpl->mutex);
    mImpl->retryRequested = true;
    mImpl->wake.notify_all();
}

void LobbySession::setInfo(LobbyInfo info) {
    std::lock_guard lock(mImpl->mutex);
    mImpl->info        = std::move(info);
    mImpl->infoChanged = true;
    mImpl->wake.notify_all();
}

} // namespace xbox_live_on_bds::xbl
