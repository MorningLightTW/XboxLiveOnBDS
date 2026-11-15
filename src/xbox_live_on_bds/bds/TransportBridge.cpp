#include "xbox_live_on_bds/bds/TransportBridge.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <utility>
#include <variant>

#include <fmt/format.h>

#include "ll/api/io/Logger.h"

#include "ll/api/memory/Hook.h"
#include "xbox_live_on_bds/net/Http.h"

#include "mc/deps/nether_net/NetworkID.h"
#include "mc/deps/nether_net/ServerNegotiator.h"
#include "mc/deps/nether_net/SignalingChannelId.h"
#include "mc/deps/nether_net/SimpleNetworkInterfaceImpl.h"
#include "mc/deps/nether_net/signaling/CandidateAdd.h"
#include "mc/deps/nether_net/signaling/ConnectError.h"
#include "mc/deps/nether_net/signaling/ConnectRequest.h"
#include "mc/deps/nether_net/signaling/ConnectResponse.h"
#include "mc/network/NetworkIdentifier.h"

namespace xbox_live_on_bds::bds::transport_bridge {

namespace {

std::atomic<NetherNet::SimpleNetworkInterfaceImpl*> transport{nullptr};

std::mutex mutex;
// NetherNet ids that came through deliver(), with when they last did; forgotten once their join is long over.
std::map<std::uint64_t, std::chrono::steady_clock::time_point> peers;
constexpr auto                                                 PeerForgetAfter = std::chrono::minutes(5);
std::function<void(std::uint64_t to, std::string signal)>      sink;
// Calls into `sink` still running outside the lock. setSink() waits for them to finish, so whoever owns what
// the old sink points at may free it as soon as setSink() returns.
int                     sinkCalls = 0;
std::condition_variable sinkIdle;

using NetworkIdBase = std::variant<std::monostate, NetherNet::P2P::NetworkID, NetherNet::Realms::NetworkID>;
static_assert(sizeof(NetworkIdBase) == sizeof(NetherNet::NetworkID));

std::optional<std::uint64_t> p2pValue(NetherNet::NetworkID const& id) {
    if (auto const* p2p = std::get_if<NetherNet::P2P::NetworkID>(&static_cast<NetworkIdBase const&>(id))) {
        return p2p->mValue;
    }
    return std::nullopt;
}

// The signal classes are opaque in the headers but laid out as { uint64 connection id; payload }, the payload
// being the SDP / candidate string, or the int error code for ConnectError.
struct SignalWithText {
    std::uint64_t connectionId;
    std::string   text;
};
struct SignalWithCode {
    std::uint64_t connectionId;
    int           code;
};
static_assert(sizeof(SignalWithText) == sizeof(NetherNet::ConnectRequest));
static_assert(sizeof(SignalWithText) == sizeof(NetherNet::ConnectResponse));
static_assert(sizeof(SignalWithText) == sizeof(NetherNet::CandidateAdd));
static_assert(sizeof(SignalWithCode) == sizeof(NetherNet::ConnectError));

// clang-format off
using Signal =
    std::variant<NetherNet::ConnectRequest, NetherNet::ConnectResponse, NetherNet::ConnectError, NetherNet::CandidateAdd>;
// clang-format on
using SendResult = NetherNet::ErrorOr<void, NetherNet::ESessionError>;

std::string serialize(Signal const& signal) {
    switch (signal.index()) {
    case 0: {
        auto const& s = reinterpret_cast<SignalWithText const&>(std::get<0>(signal));
        return fmt::format("CONNECTREQUEST {} {}", s.connectionId, s.text);
    }
    case 1: {
        auto const& s = reinterpret_cast<SignalWithText const&>(std::get<1>(signal));
        return fmt::format("CONNECTRESPONSE {} {}", s.connectionId, s.text);
    }
    case 2: {
        auto const& s = reinterpret_cast<SignalWithCode const&>(std::get<2>(signal));
        return fmt::format("CONNECTERROR {} {}", s.connectionId, s.code);
    }
    case 3: {
        auto const& s = reinterpret_cast<SignalWithText const&>(std::get<3>(signal));
        return fmt::format("CANDIDATEADD {} {}", s.connectionId, s.text);
    }
    default:
        return {};
    }
}

// Every signal the transport takes in passes here: remember the transport.
LL_TYPE_INSTANCE_HOOK(
    CaptureTransportHook,
    ll::memory::HookPriority::Normal,
    NetherNet::SimpleNetworkInterfaceImpl,
    &NetherNet::SimpleNetworkInterfaceImpl::ReceiveFromSignalingChannel,
    void,
    NetherNet::NetworkID          from,
    std::string_view              message,
    NetherNet::SignalingChannelId sourceChannel
) {
    transport.store(this);
    origin(from, message, sourceChannel);
}

// Everything the transport sends: copy what is meant for our peers to the sink. The original still runs (its
// return type is opaque here, so it is passed through untouched rather than made up).
LL_TYPE_INSTANCE_HOOK(
    ForwardToPeersHook,
    ll::memory::HookPriority::Normal,
    NetherNet::SimpleNetworkInterfaceImpl,
    &NetherNet::SimpleNetworkInterfaceImpl::SendToSignalingChannel,
    SendResult,
    NetherNet::NetworkID                         networkIDTo,
    Signal const&                                signal,
    std::optional<NetherNet::SignalingChannelId> preference
) {
    transport.store(this);
    if (auto to = p2pValue(networkIDTo)) {
        std::function<void(std::uint64_t, std::string)> target;
        {
            std::lock_guard lock(mutex);
            if (peers.contains(*to) && sink) {
                target = sink;
                ++sinkCalls;
            }
        }
        if (target) {
            struct Done {
                ~Done() {
                    {
                        std::lock_guard lock(mutex);
                        --sinkCalls;
                    }
                    sinkIdle.notify_all();
                }
            } done;
            target(*to, serialize(signal));
        }
    }
    return origin(networkIDTo, signal, preference);
}

std::vector<RelayServer> relayServers; // guarded by mutex
ll::io::Logger*          logger = nullptr;
std::atomic<bool>        reportedRelaysOnly{false};

// Connection ids of the CONNECTREQUESTs deliver() handed in: Xbox Live friends' joins, as opposed to joins
// that reach BDS some other way (LAN, NetherNet by address). Guarded by mutex; anyone can send a CONNECTREQUEST,
// so the table expires and is capped.
std::map<std::uint64_t, std::chrono::steady_clock::time_point> xboxJoins;
constexpr std::size_t                                          MaxXboxJoins = 256;

// Friends' signals deliver() holds back until the first relays arrive, in arrival order. Guarded by mutex. A join
// waits this long, as go-nethernet's listener waits for credentials; then it is dropped and the friend's game
// gives up on its own.
struct HeldSignal {
    std::uint64_t                         from;
    std::string                           signal;
    std::chrono::steady_clock::time_point at;
};
std::vector<HeldSignal> held;

// NetherNet ids of friends whose CONNECTREQUEST deliver() injected: who joined through Xbox Live. A mark lasts one
// stay: it goes when that player leaves (a client keeps its id, so a later join by address must not inherit it),
// or, for a join that never got into the game, after PeerForgetAfter. Guarded by mutex.
struct XboxPeer {
    std::chrono::steady_clock::time_point at;
    bool                                  inGame = false;
};
std::map<std::uint64_t, XboxPeer> xboxPeers;
constexpr std::size_t             MaxXboxPeers = 1024;
constexpr auto                    HoldLimit    = std::chrono::seconds(15);
constexpr std::size_t             MaxHeld      = 512;

/// The connection id of "CONNECTREQUEST <connection id> <offer>"; empty for other signals.
std::optional<std::uint64_t> connectRequestId(std::string_view signal) {
    constexpr std::string_view Prefix = "CONNECTREQUEST ";
    if (!signal.starts_with(Prefix)) return std::nullopt;
    signal.remove_prefix(Prefix.size());
    std::uint64_t id{};
    auto [end, ec] = std::from_chars(signal.data(), signal.data() + signal.size(), id);
    if (ec != std::errc{} || end == signal.data()) return std::nullopt;
    return id;
}

using PeerConnectionResult =
    Bedrock::Result<webrtc::scoped_refptr<webrtc::PeerConnectionInterface>, NetherNet::ESessionError>;
using AnswerCallback = brstd::move_only_function<void(PeerConnectionResult)>;

// Every peer connection BDS answers passes here, after LeviLamina's own patch (High), which appends the STUN
// servers of its config. A friend's join through Xbox Live gets the ICE servers a world hosted by the game gets:
// only the relays the signaling service handed out (every UDP TURN server also answers STUN), as go-nethernet
// and gophertunnel do too. Joins from elsewhere keep the server's own configuration. A friend's join reaches BDS
// only once there are relays (deliver() holds it until then), so the fallback below is for the unexpected.
LL_TYPE_INSTANCE_HOOK(
    AddRelayServersHook,
    ll::memory::HookPriority::Low,
    NetherNet::ServerNegotiator,
    &NetherNet::ServerNegotiator::$createAnswer,
    void,
    webrtc::PeerConnectionInterface::RTCConfiguration const& config,
    NetherNet::ConnectRequest const&                         offer,
    AnswerCallback&&                                         onComplete
) {
    auto connectionId = reinterpret_cast<SignalWithText const&>(offer).connectionId;
    bool xboxJoin = false, relaysOnly = false;
    {
        std::lock_guard lock(mutex);
        xboxJoin = xboxJoins.erase(connectionId) > 0;
        if (xboxJoin && !relayServers.empty()) {
            auto& servers = const_cast<webrtc::PeerConnectionInterface::RTCConfiguration&>(config).servers.get();
            servers.clear();
            for (auto const& relay : relayServers) {
                webrtc::PeerConnectionInterface::IceServer server{};
                server.urls     = relay.urls;
                server.username = relay.username;
                server.password = relay.password;
                servers.push_back(std::move(server));
            }
            relaysOnly = true;
        }
    }
    if (logger) {
        if (relaysOnly && !reportedRelaysOnly.exchange(true)) {
            logger->info(
                "Friends joining through Xbox Live use only Microsoft's relay servers, like a world the game hosts"
            );
        } else if (xboxJoin && !relaysOnly) {
            logger->warn(
                "No relay servers for a friend's join (connection {}); it uses the server's own ICE servers",
                connectionId
            );
        }
    }
    origin(config, offer, std::move(onComplete));
}

/// Hands a signal to the transport as if it came over its own WebSocket signaling channel.
void inject(NetherNet::SimpleNetworkInterfaceImpl& target, std::uint64_t from, std::string_view signal) {
    alignas(NetherNet::NetworkID) std::byte storage[sizeof(NetherNet::NetworkID)];
    auto* base = new (storage) NetworkIdBase(std::in_place_index<1>, NetherNet::P2P::NetworkID{from});
    target.ReceiveFromSignalingChannel(
        *reinterpret_cast<NetherNet::NetworkID*>(storage),
        signal,
        NetherNet::SignalingChannelId::WebSocket
    );
    std::destroy_at(base);
}

/// Drops held signals older than HoldLimit (with the lock held); returns how many joins that gave up on.
std::size_t dropStaleHeld(std::chrono::steady_clock::time_point now) {
    std::size_t joins = 0;
    std::erase_if(held, [&](HeldSignal const& entry) {
        if (now - entry.at <= HoldLimit) return false;
        if (connectRequestId(entry.signal)) ++joins;
        return true;
    });
    return joins;
}

void reportDropped(std::size_t joins) {
    if (joins > 0 && logger) {
        logger->warn(
            "{} friend join(s) gave up: Xbox Live sent no relay servers within {} s",
            joins,
            std::chrono::duration_cast<std::chrono::seconds>(HoldLimit).count()
        );
    }
}

} // namespace

void install(ll::io::Logger& log) {
    logger = &log;
    static ll::memory::HookRegistrar<CaptureTransportHook, ForwardToPeersHook, AddRelayServersHook> registrar;
}

void setRelayServers(std::vector<RelayServer> servers) {
    std::vector<HeldSignal> release;
    std::size_t             dropped = 0;
    {
        std::lock_guard lock(mutex);
        relayServers = std::move(servers);
        dropped      = dropStaleHeld(std::chrono::steady_clock::now());
        if (!relayServers.empty()) release = std::exchange(held, {});
    }
    reportDropped(dropped);
    // Outside the lock: the transport may answer at once, and its replies pass ForwardToPeersHook, which takes it.
    auto* target = transport.load();
    if (!target || release.empty()) return;
    if (logger) logger->debug("Relay servers arrived; passing {} held signal(s) to the server", release.size());
    for (auto const& entry : release) inject(*target, entry.from, entry.signal);
}

bool ready() { return transport.load() != nullptr; }

namespace {
std::optional<std::uint64_t> netherNetIdOf(NetworkIdentifier const& player) {
    if (player.mType != NetworkIdentifier::Type::NetherNet) return std::nullopt;
    return p2pValue(player.mNetherNetId);
}
} // namespace

bool joinedThroughXboxLive(NetworkIdentifier const& player) {
    auto id = netherNetIdOf(player);
    if (!id) return false;
    std::lock_guard lock(mutex);
    return xboxPeers.contains(*id);
}

void onPlayerJoined(NetworkIdentifier const& player) {
    auto id = netherNetIdOf(player);
    if (!id) return;
    std::lock_guard lock(mutex);
    if (auto it = xboxPeers.find(*id); it != xboxPeers.end()) it->second.inGame = true;
}

void onPlayerLeft(NetworkIdentifier const& player) {
    auto id = netherNetIdOf(player);
    if (!id) return;
    std::lock_guard lock(mutex);
    xboxPeers.erase(*id);
}

bool prime(std::uint16_t port) {
    if (ready()) return true;
    // A NetherNet id no friend has (theirs are random 64-bit values); BDS's reply to it goes nowhere.
    std::uint64_t probeId = std::random_device{}() | 1;
    auto          res     = net::request(
        "POST",
        fmt::format("http://127.0.0.1:{}/v1/join/{}", port, probeId),
        {
            {"Content-Type", "application/sdp"}
    },
        "v=0"
    );
    if (ready()) {
        if (logger) logger->debug("Reached the server's NetherNet transport");
        return true;
    }
    if (logger) {
        logger->warn(
            "Could not reach the server's NetherNet transport ({}); the first friend to join may not get in",
            res.describe()
        );
    }
    return false;
}

bool deliver(std::uint64_t from, std::string_view signal) {
    auto* target = transport.load();
    if (!target) return false;
    bool        hold    = false;
    std::size_t dropped = 0;
    {
        std::lock_guard lock(mutex);
        auto            now = std::chrono::steady_clock::now();
        std::erase_if(peers, [&](auto const& entry) { return now - entry.second > PeerForgetAfter; });
        peers[from] = now;
        auto joinId = connectRequestId(signal);
        if (joinId) {
            std::erase_if(xboxJoins, [&](auto const& entry) { return now - entry.second > PeerForgetAfter; });
            if (xboxJoins.size() < MaxXboxJoins || xboxJoins.contains(*joinId)) xboxJoins[*joinId] = now;
            std::erase_if(xboxPeers, [&](auto const& entry) {
                return !entry.second.inGame && now - entry.second.at > PeerForgetAfter;
            });
            if (auto it = xboxPeers.find(from); it != xboxPeers.end()) {
                it->second.at = now;
            } else if (xboxPeers.size() < MaxXboxPeers) {
                xboxPeers[from] = {now};
            }
        }
        // Before the first relays, a friend's join waits for them instead of falling back to the server's own ICE
        // servers, as the game's host does; that peer's later signals (its candidates) wait behind it, in order.
        dropped = dropStaleHeld(now);
        if (relayServers.empty()
            && (joinId || std::ranges::any_of(held, [&](HeldSignal const& entry) { return entry.from == from; }))) {
            hold = true;
            if (held.size() < MaxHeld) held.push_back({from, std::string(signal), now});
        }
    }
    reportDropped(dropped);
    if (hold) {
        if (logger) logger->debug("Holding a friend's signal until Xbox Live sends relay servers");
        return true;
    }
    inject(*target, from, signal);
    return true;
}

void setSink(std::function<void(std::uint64_t to, std::string signal)> newSink) {
    std::unique_lock lock(mutex);
    sink = std::move(newSink);
    // Not from inside a sink call: it would wait for itself.
    sinkIdle.wait(lock, [] { return sinkCalls == 0; });
}

} // namespace xbox_live_on_bds::bds::transport_bridge
