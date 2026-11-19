#include "xbox_live_on_bds/xbl/Signaling.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "ll/api/io/Logger.h"

#include "xbox_live_on_bds/ice/StandIn.h"
#include "xbox_live_on_bds/net/Http.h"
#include "xbox_live_on_bds/net/WebSocket.h"
#include "xbox_live_on_bds/util/Encoding.h"
#include "xbox_live_on_bds/util/Redact.h"
#include "xbox_live_on_bds/xbl/SignalText.h"

namespace xbox_live_on_bds::xbl {

using nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::string_view SignalingUrl = "wss://signal.franchise.minecraft-services.net/ws/v1.0/messaging/connect";
constexpr std::string_view UserAgent    = "libHttpClient/1.0.0.0";

constexpr std::string_view MethodTurnAuth    = "Signaling_TurnAuth_v1_0";
constexpr std::string_view MethodSendMessage = "Signaling_SendClientMessage_v1_0";
constexpr std::string_view MethodReceive     = "Signaling_ReceiveMessage_v1_0";
constexpr std::string_view MethodPing        = "System_Ping_v1_0";
constexpr std::string_view MethodPong        = "System_Pong_v1_0";
constexpr std::string_view InnerWebRtc       = "Signaling_WebRtc_v1_0";
constexpr std::string_view InnerDelivery     = "Signaling_DeliveryNotification_V1_0";

constexpr std::string_view ConnectRequest  = "CONNECTREQUEST";
constexpr std::string_view ConnectResponse = "CONNECTRESPONSE";
constexpr std::string_view CandidateAdd    = "CANDIDATEADD";
constexpr std::string_view ConnectError    = "CONNECTERROR";

// The game's InitialConnection-XX codes (DisconnectFailReason).
constexpr int ErrorServerNotFound = 1; // NetherNetSessionNotFound
constexpr int ErrorConnectRequest = 6; // NetherNetConnectRequest

// Friends trickle their ICE candidates; BDS's HTTP signaling wants the complete offer at once.
constexpr auto GatherQuiet = 500ms;  // after the last candidate
constexpr auto GatherMax   = 3000ms; // after the offer
constexpr auto PingEvery   = 50s;
// The service answers every ping; this long without a word means the connection died without closing (a
// network change, a sleeping NAT mapping), and friends' joins would go nowhere until it is replaced.
constexpr auto SilenceLimit    = 4min;
constexpr auto PeerForgetAfter = 5min; // since a friend's last signal
constexpr auto TurnAuthRetry   = 1min; // when a TurnAuth request got no answer
// Each stand-in is a UDP socket and a thread, and anyone can send a CONNECTREQUEST: cap what they can open.
constexpr std::size_t MaxStandIns = 32;
// Each player entry is kept for minutes and anyone can send signals: cap how many they can fill.
constexpr std::size_t MaxPlayers = 256;
// Joins relayed over BDS's HTTP signaling (only while its transport cannot be reached) run one after another, and
// BDS takes 10 s to give up on one it cannot reach: cap what anyone can queue, and skip a join that waited behind
// others for longer than the friend's game does.
constexpr std::size_t MaxOffers          = 16;
constexpr std::size_t MaxOfferCandidates = 64;
constexpr auto        OfferStaleAfter    = 8s;
// BDS's replies waiting for the sender thread; a full queue means the connection is stuck, and they are dropped.
constexpr std::size_t MaxOutbox = 256;

struct PendingOffer {
    std::string              sender;            // Signaling player id of the friend's game.
    std::string              senderNetherNetId; // Its NetherNet id: what the game puts in /v1/join/<id> itself.
    std::string              sdp;
    std::vector<std::string> candidates;
    Clock::time_point        received;
    Clock::time_point        lastCandidate;
};

} // namespace

struct Signaling::Impl {
    ll::io::Logger&                 logger;
    Options                         options;
    std::unique_ptr<net::WebSocket> socket;

    std::mutex                          mutex;
    std::condition_variable_any         wake;
    std::map<std::string, PendingOffer> offers; // by connection id (HTTP relay only)
    struct Player {
        std::string       id;       // signaling player id
        Clock::time_point lastSeen; // last signal from them
    };
    std::map<std::uint64_t, Player> players;    // by friend's NetherNet id; dropped once their join is long over
    std::string                     turnAuthId; // Request id of the latest Signaling_TurnAuth_v1_0; guarded by mutex
    Clock::time_point               turnRefreshAt = Clock::time_point::max(); // guarded by mutex
    std::atomic<bool>               turnReported{false}; // first grant logs at info; refreshes and reconnects at debug

    void requestTurnAuth() {
        std::string id = util::randomUuid();
        {
            std::lock_guard lock(mutex);
            turnAuthId    = id;                           // Known before any response can arrive.
            turnRefreshAt = Clock::now() + TurnAuthRetry; // Ask again if no answer comes.
        }
        request(MethodTurnAuth, json::object(), id);
    }

    /// TURN relays for the server, the same ones the service hands the game for hosting. They expire
    /// (ExpirationInSeconds, as go-nethernet's Credentials reads it): ask again before they do, or friends behind
    /// NAT stop getting through on a server that has been up for long.
    void onTurnAuth(json const& result) {
        auto lifetime = std::chrono::seconds(result.value("ExpirationInSeconds", 0));
        {
            std::lock_guard lock(mutex);
            // No expiry given: keep these for the life of the connection, as before.
            turnRefreshAt = lifetime > 0s ? Clock::now() + std::max<Clock::duration>(lifetime * 4 / 5, 30s)
                                          : Clock::time_point::max();
        }
        std::vector<TurnServer> servers;
        for (auto const& entry : result.value("TurnAuthServers", json::array())) {
            TurnServer server{{}, entry.value("Username", ""), entry.value("Password", "")};
            for (auto const& url : entry.value("Urls", json::array())) {
                if (url.is_string()) server.urls.push_back(url.get<std::string>());
            }
            if (!server.urls.empty()) servers.push_back(std::move(server));
        }
        auto level = turnReported.exchange(true) ? ll::io::LogLevel::Debug : ll::io::LogLevel::Info;
        if (lifetime > 0s) {
            logger.log(
                level,
                "Got {} TURN relay server(s) for friends behind NAT, valid for {} min",
                servers.size(),
                std::chrono::duration_cast<std::chrono::minutes>(lifetime).count()
            );
        } else {
            logger.log(level, "Got {} TURN relay server(s) for friends behind NAT", servers.size());
        }
        if (options.onTurnServers) options.onTurnServers(std::move(servers));
    }
    bool                    closedReported = false;
    std::atomic<Clock::rep> lastHeard{Clock::now().time_since_epoch().count()};

    struct StandInEntry {
        std::unique_ptr<ice::StandIn> standIn;
        Clock::time_point             opened;
        bool                          reported = false;
    };
    std::map<std::string, StandInEntry> standIns; // by connection id; guarded by mutex

    static std::string sdpValue(std::string_view sdp, std::string_view key) {
        auto at = sdp.find(key);
        if (at == std::string_view::npos) return {};
        at       += key.size();
        auto end  = sdp.find_first_of("\r\n", at);
        return std::string(sdp.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
    }

    /// Tell BDS the friend can also be reached at a local stand-in that answers its checks (see ice::StandIn).
    void addStandIn(std::uint64_t peer, std::string const& connectionId, std::string_view offer) {
        auto ufrag    = sdpValue(offer, "a=ice-ufrag:");
        auto password = sdpValue(offer, "a=ice-pwd:");
        if (ufrag.empty() || password.empty()) return;
        {
            std::lock_guard lock(mutex);
            if (standIns.size() >= MaxStandIns && !standIns.contains(connectionId)) {
                logger.debug(
                    "Too many stand-ins open; not adding one for connection {}",
                    util::Redactor::instance().id(connectionId)
                );
                return;
            }
        }
        auto standIn = ice::StandIn::open(ufrag, password);
        if (!standIn) {
            logger.warn("Could not open a local stand-in for a friend's join");
            return;
        }
        std::vector<std::string> addresses{"127.0.0.1"};
        if (auto lan = ice::primaryIpv4(); !lan.empty()) addresses.push_back(lan);
        int foundation = 1;
        for (auto const& address : addresses) {
            options.inject(
                peer,
                fmt::format(
                    "{} {} candidate:{} 1 udp {} {} {} typ host generation 0 ufrag {} network-id {} network-cost 10",
                    CandidateAdd,
                    connectionId,
                    3000000000u + foundation,
                    2130706431 - foundation, // ahead of the friend's own candidates
                    address,
                    standIn->port(),
                    ufrag,
                    90 + foundation
                )
            );
            ++foundation;
        }
        // A replaced entry dies after the unlock: ~StandIn joins its thread, and fromTransport (on the game's
        // network thread) takes this mutex.
        StandInEntry replaced;
        {
            std::lock_guard lock(mutex);
            auto&           slot = standIns[connectionId];
            replaced             = std::move(slot);
            slot                 = {std::move(standIn), Clock::now()};
        }
    }

    /// Reports stand-ins that got checked, and closes them after a minute.
    void tendStandIns() {
        std::vector<StandInEntry> expired; // destroyed after the unlock: ~StandIn joins its thread
        {
            std::lock_guard lock(mutex);
            auto            now = Clock::now();
            for (auto it = standIns.begin(); it != standIns.end();) {
                auto& entry = it->second;
                if (!entry.reported && entry.standIn->checksAnswered() > 0) {
                    logger.debug(
                        "Answered the server's connectivity check for a friend (connection {})",
                        util::Redactor::instance().id(it->first)
                    );
                    entry.reported = true;
                }
                if (now - entry.opened > 60s) {
                    expired.push_back(std::move(entry));
                    it = standIns.erase(it);
                } else {
                    ++it;
                }
            }
            // A join is negotiated within seconds; after that BDS has nothing more to signal to the friend.
            std::erase_if(players, [&](auto const& entry) { return now - entry.second.lastSeen > PeerForgetAfter; });
        }
    }

    /// A signal from BDS's transport to a friend: back over Xbox signaling to that friend's game.
    /// Runs on BDS's own thread, which must not wait for the network: the signal is only queued here.
    void fromTransport(std::uint64_t to, std::string signal) {
        {
            std::lock_guard lock(mutex);
            if (closedReported) return;
            auto it = players.find(to);
            if (it == players.end() || outbox.size() >= MaxOutbox) return;
            outbox.emplace_back(it->second.id, std::move(signal));
        }
        sendWake.notify_one();
    }

    /// Sends what fromTransport queued, in order.
    void sendLoop(std::stop_token const& stop) {
        for (;;) {
            std::deque<std::pair<std::string, std::string>> batch;
            {
                std::unique_lock lock(mutex);
                sendWake.wait(lock, stop, [this] { return !outbox.empty(); });
                if (stop.stop_requested()) return;
                batch.swap(outbox);
            }
            for (auto const& [player, signal] : batch) {
                if (signal.starts_with(ConnectResponse)) logger.debug("The server answered a friend's join");
                else if (signal.starts_with(ConnectError))
                    logger.warn("The server refused a friend's join: {}", signal);
                sendSignal(player, signal);
            }
        }
    }

    std::deque<std::pair<std::string, std::string>> outbox; // (signaling player id, signal); guarded by mutex
    std::condition_variable_any                     sendWake;

    std::jthread receiver;
    std::jthread ticker;
    std::jthread sender;

    Impl(ll::io::Logger& logger, Options options) : logger(logger), options(std::move(options)) {
        turnReported = this->options.reconnect;
    }

    ~Impl() {
        if (options.setInjectSink) options.setInjectSink({});
        {
            std::lock_guard lock(mutex);
            closedReported = true; // Closing on purpose: not a lost connection.
        }
        ticker.request_stop();
        sender.request_stop();
        if (socket) socket->close();
        if (receiver.joinable()) receiver.join();
        if (ticker.joinable()) ticker.join();
        if (sender.joinable()) sender.join();
    }

    // ---- JSON-RPC -----------------------------------------------------------------------------------------

    std::string request(std::string_view method, json params, std::string id = util::randomUuid()) {
        socket->send(
            json{
                {"jsonrpc", "2.0"            },
                {"method",  method           },
                {"id",      id               },
                {"params",  std::move(params)}
        }.dump()
        );
        return id;
    }

    void respond(json const& id) {
        socket->send(
            json{
                {"jsonrpc", "2.0"  },
                {"id",      id     },
                {"result",  nullptr}
        }.dump()
        );
    }

    void sendToPlayer(std::string const& to, json const& inner) {
        request(
            MethodSendMessage,
            {
                {"toPlayerId", to                },
                {"messageId",  util::randomUuid()},
                {"message",    inner.dump()      }
        }
        );
    }

    void sendSignal(std::string const& to, std::string const& signal) {
        sendToPlayer(
            to,
            {
                {"jsonrpc", "2.0"                                                                      },
                {"method",  InnerWebRtc                                                                },
                {"params",  {{"netherNetId", std::to_string(options.netherNetId)}, {"message", signal}}}
        }
        );
    }

    // ---- incoming -----------------------------------------------------------------------------------------

    void onSignal(std::string const& from, std::string const& fromNetherNetId, std::string_view raw) {
        // The connection id and the NetherNet id are the remote player's own words, and they end up in tables, in
        // the log and in a URL to BDS: numbers, or the signal is dropped.
        auto signal = parseSignal(raw);
        auto claim  = parseNetherNetId(fromNetherNetId);
        if (!signal || !claim) {
            logger.debug("Ignoring a signal that is not a NetherNet signal");
            return;
        }
        std::string   type(signal->type);
        std::string   connectionId(signal->connectionId);
        std::string   payload(signal->payload);
        std::uint64_t peer = *claim;

        // Straight into BDS's transport, every signal as it comes (trickle ICE), when it can be reached.
        if (options.inject) {
            // `from` is set by the signaling service, but the NetherNet id is the sender's own claim: the first
            // player to use an id keeps it, or anyone could take over a friend's join and receive its replies.
            const char* rejected = nullptr;
            {
                std::lock_guard lock(mutex);
                auto            now = Clock::now();
                if (auto it = players.find(peer); it != players.end()) {
                    if (it->second.id == from) it->second.lastSeen = now;
                    else rejected = "Ignoring a signal that claims another player's NetherNet id";
                } else if (players.size() >= MaxPlayers) {
                    rejected = "Ignoring a signal because the player table is full";
                } else {
                    players[peer] = {from, now};
                }
            }
            if (rejected) {
                logger.debug("{}", util::Redactor::instance().text(rejected));
                return;
            }
            if (options.inject(peer, raw)) {
                if (type == ConnectRequest) {
                    logger.debug(
                        "Passing a friend's join to the server (connection {})",
                        util::Redactor::instance().id(connectionId)
                    );
                    addStandIn(peer, connectionId, payload);
                }
                return;
            }
        }

        std::lock_guard lock(mutex);
        if (type == ConnectRequest) {
            if (offers.size() >= MaxOffers && !offers.contains(connectionId)) {
                logger.debug(
                    "Too many joins waiting to be relayed; dropping connection {}",
                    util::Redactor::instance().id(connectionId)
                );
                return;
            }
            auto now             = Clock::now();
            offers[connectionId] = PendingOffer{from, fromNetherNetId, std::move(payload), {}, now, now};
            wake.notify_all();
        } else if (type == CandidateAdd) {
            if (auto it = offers.find(connectionId);
                it != offers.end() && it->second.candidates.size() < MaxOfferCandidates) {
                it->second.candidates.push_back(std::move(payload));
                it->second.lastCandidate = Clock::now();
            }
        } else if (type == ConnectError) {
            offers.erase(connectionId);
            logger.debug(
                "Join {} was abandoned by the player's game: {}",
                util::Redactor::instance().id(connectionId),
                util::Redactor::instance().text(payload)
            );
        }
    }

    void onMessage(json const& message) {
        if (!message.is_object()) return;
        std::string from = message.value("From", "");
        std::string text = message.value("Message", "");
        if (from.empty()) return;
        // Delivery receipt, as the game sends one.
        sendToPlayer(
            from,
            {
                {"jsonrpc", "2.0"                                                   },
                {"method",  InnerDelivery                                           },
                {"params",  {{"messageId", message.value("Id", util::randomUuid())}}}
        }
        );
        auto inner = json::parse(text, nullptr, false);
        if (inner.is_object() && inner.value("method", "") == InnerWebRtc) {
            auto const& params = inner.value("params", json::object());
            auto        id     = params.value("netherNetId", json());
            onSignal(
                from,
                id.is_string()   ? id.get<std::string>()
                : id.is_number() ? id.dump()
                                 : std::string{},
                params.value("message", std::string{})
            );
        }
    }

    void handle(std::string const& text) {
        auto j = json::parse(text, nullptr, false);
        if (!j.is_object()) return;
        if (j.contains("error") && !j["error"].is_null()) {
            logger.warn("Signaling service error: {}", j["error"].dump());
            return;
        }
        if (!j.contains("method")) { // A response to one of our requests.
            bool isTurnAuth;
            {
                std::lock_guard lock(mutex);
                isTurnAuth = j.value("id", json()) == turnAuthId;
            }
            if (isTurnAuth) onTurnAuth(j.value("result", json::object()));
            return;
        }
        std::string method = j["method"].get<std::string>();
        json        id     = j.value("id", json());
        if (method == MethodReceive) {
            if (!id.is_null()) respond(id);
            json const& params = j.value("params", json());
            if (params.is_array()) {
                for (auto const& message : params) onMessage(message);
            } else {
                onMessage(params);
            }
        } else if (method == MethodPing || method == MethodPong) {
            if (!id.is_null()) respond(id);
        }
    }

    void receiveLoop() {
        while (auto text = socket->receive()) {
            lastHeard = Clock::now().time_since_epoch().count();
            // Content comes from the service and, inside onMessage, from any remote player: a wrong type
            // must drop the message, not end the process (an exception out of this thread terminates it).
            try {
                handle(*text);
            } catch (std::exception const& e) {
                logger.debug("Ignoring a malformed signaling message: {}", util::Redactor::instance().text(e.what()));
            }
        }
        bool report = false;
        {
            std::lock_guard lock(mutex);
            report         = !closedReported;
            closedReported = true;
            wake.notify_all();
        }
        if (report && options.onClosed) options.onClosed(socket->closeReason());
    }

    // ---- relay into BDS ------------------------------------------------------------------------------------

    /// "a=candidate:<foundation> <component> <transport> <priority> <address> ..." with an IPv6 address.
    static bool isIpv6Candidate(std::string_view line) {
        if (!line.starts_with("a=candidate:")) return false;
        std::size_t field = 0, start = 0;
        for (std::size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == ' ') {
                if (field == 4) return line.substr(start, i - start).find(':') != std::string_view::npos;
                ++field;
                start = i + 1;
            }
        }
        return false;
    }

    /// The offer with its trickled candidates folded in (BDS's HTTP signaling takes no trickle), as
    /// "\r\n"-terminated lines, without IPv6 candidates: players are relayed over IPv4, as by MCXboxBroadcast.
    static std::string completeOffer(PendingOffer const& offer) {
        std::vector<std::string> lines;
        std::string_view         rest = offer.sdp;
        while (!rest.empty()) {
            auto        end = rest.find('\n');
            std::string line(rest.substr(0, end));
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) lines.push_back(std::move(line));
        }
        for (auto candidate : offer.candidates) {
            while (!candidate.empty() && (candidate.back() == '\r' || candidate.back() == '\n')) candidate.pop_back();
            if (candidate.empty()) continue;
            lines.push_back(candidate.starts_with("a=") ? candidate : "a=" + candidate);
        }
        std::string sdp;
        for (auto const& line : lines) {
            if (isIpv6Candidate(line)) continue;
            sdp += line;
            sdp += "\r\n";
        }
        return sdp;
    }

    void dump(std::string const& name, std::string_view content) const {
        if (options.debugDir.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(options.debugDir, ec);
        std::ofstream(options.debugDir / name, std::ios::binary | std::ios::trunc) << content;
    }

    void relay(std::string const& connectionId, PendingOffer offer) {
        std::string sdp = completeOffer(offer);
        dump("last-offer.sdp", util::Redactor::instance().sdp(sdp));
        // The game joins a NetherNet server as itself: /v1/join/<its own NetherNet id> (digits only, see onSignal).
        std::string const& joinId = offer.senderNetherNetId;
        dump(
            "last-ids.txt",
            fmt::format(
                "connection {}\nsender netherNetId {}\nsender player {}\n",
                util::Redactor::instance().id(connectionId),
                util::Redactor::instance().id(offer.senderNetherNetId),
                util::Redactor::instance().player(offer.sender)
            )
        );

        auto started = Clock::now();
        // Status check first, the way the game and MCXboxBroadcast reach HTTP signaling.
        auto probe = net::request(
            "GET",
            fmt::format("{}/v1/join?id={}", options.relayBaseUrl, joinId),
            {
                {"Accept", "application/json"}
        }
        );
        if (!probe.ok()) logger.warn("BDS's NetherNet status check failed: {}", probe.describe());
        auto res = net::request(
            "POST",
            fmt::format("{}/v1/join/{}", options.relayBaseUrl, joinId),
            {
                {"Content-Type", "application/sdp"},
                {"Accept",       "application/sdp"}
        },
            sdp
        );
        dump(
            "last-answer.txt",
            fmt::format(
                "{}\n\n{}",
                util::Redactor::instance().text(res.describe()),
                util::Redactor::instance().sdp(res.body)
            )
        );
        if (!res.ok() || res.body.empty()) {
            int code = res.error.empty() ? ErrorConnectRequest : ErrorServerNotFound;
            logger.error(
                "BDS refused a friend's join after {} ms: {}",
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count(),
                res.describe()
            );
            logger.debug("BDS refused the join of connection {}", util::Redactor::instance().id(connectionId));
            sendSignal(offer.sender, fmt::format("{} {} {}", ConnectError, connectionId, code));
            return;
        }
        sendSignal(offer.sender, fmt::format("{} {} {}", ConnectResponse, connectionId, res.body));
        logger.info(
            "Relayed a friend's join to the server in {} ms ({} trickled candidate(s))",
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count(),
            offer.candidates.size()
        );
    }

    void tickLoop(std::stop_token const& stop) {
        auto nextPing = Clock::now() + PingEvery;
        while (!stop.stop_requested()) {
            std::vector<std::pair<std::string, PendingOffer>> ready;
            bool                                              refreshTurn = false;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, stop, 100ms, [] { return false; });
                if (closedReported) return;
                auto now    = Clock::now();
                refreshTurn = now >= turnRefreshAt;
                for (auto it = offers.begin(); it != offers.end();) {
                    if (now - it->second.lastCandidate >= GatherQuiet || now - it->second.received >= GatherMax) {
                        ready.emplace_back(it->first, std::move(it->second));
                        it = offers.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (refreshTurn) requestTurnAuth();
            for (auto& [connectionId, offer] : ready) {
                if (Clock::now() - offer.received > OfferStaleAfter) {
                    logger.debug(
                        "Join {} waited too long behind others to be relayed; dropping it",
                        util::Redactor::instance().id(connectionId)
                    );
                    continue;
                }
                relay(connectionId, std::move(offer));
            }
            tendStandIns();
            if (Clock::now() - Clock::time_point(Clock::duration(lastHeard.load())) > SilenceLimit) {
                // receiveLoop ends and reports the loss with this reason.
                socket->close(
                    fmt::format(
                        "no word from the service for {} min",
                        std::chrono::duration_cast<std::chrono::minutes>(SilenceLimit).count()
                    )
                );
                return;
            }
            if (Clock::now() >= nextPing) {
                request(MethodPing, json::object());
                nextPing = Clock::now() + PingEvery;
            }
        }
    }
};

std::unique_ptr<Signaling> Signaling::connect(ll::io::Logger& logger, Options options, std::string& error) {
    auto impl    = std::make_unique<Impl>(logger, std::move(options));
    impl->socket = net::WebSocket::connect(
        SignalingUrl,
        {
            {"Authorization", impl->options.minecraftAuthorization},
            {"User-Agent",    std::string(UserAgent)              },
            {"session-id",    util::randomUuid()                  },
            {"request-id",    util::randomUuid()                  }
    },
        error
    );
    if (!impl->socket) return nullptr;
    Impl* raw = impl.get();
    if (impl->options.setInjectSink) {
        impl->options.setInjectSink([raw](std::uint64_t to, std::string signal) {
            raw->fromTransport(to, std::move(signal));
        });
    }
    impl->sender   = std::jthread([raw](std::stop_token stop) { raw->sendLoop(stop); });
    impl->receiver = std::jthread([raw] { raw->receiveLoop(); });
    impl->ticker   = std::jthread([raw](std::stop_token stop) { raw->tickLoop(stop); });
    // The game asks for TURN credentials first; the service may expect it of every client.
    raw->requestTurnAuth();
    return std::unique_ptr<Signaling>(new Signaling(std::move(impl)));
}

Signaling::Signaling(std::unique_ptr<Impl> impl) : mImpl(std::move(impl)) {}

Signaling::~Signaling() = default;

bool Signaling::isOpen() const { return mImpl->socket->isOpen(); }

} // namespace xbox_live_on_bds::xbl
