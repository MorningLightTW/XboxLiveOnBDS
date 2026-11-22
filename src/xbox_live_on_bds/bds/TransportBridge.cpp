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
#include <type_traits>
#include <utility>
#include <variant>

#include <fmt/format.h>

#include "ll/api/Versions.h"
#include "ll/api/io/Logger.h"

#include "ll/api/memory/Hook.h"
#include "xbox_live_on_bds/bds/SignalLayout.h"
#include "xbox_live_on_bds/net/Http.h"
#include "xbox_live_on_bds/util/Redact.h"

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

// Reading uses the base class the headers declare. Building still needs placement new of that base, because the
// NetworkID constructors have no address.
using NetworkIdBase = std::variant<std::monostate, NetherNet::P2P::NetworkID, NetherNet::Realms::NetworkID>;
static_assert(
    std::is_base_of_v<NetworkIdBase, NetherNet::NetworkID> && sizeof(NetworkIdBase) == sizeof(NetherNet::NetworkID)
);

std::optional<std::uint64_t> p2pValue(NetherNet::NetworkID const& id) {
    if (auto const* p2p = std::get_if<NetherNet::P2P::NetworkID>(&id)) {
        return p2p->mValue;
    }
    return std::nullopt;
}

using layout::SignalWithCode;
using layout::SignalWithText;

// clang-format off
using Signal =
    std::variant<NetherNet::ConnectRequest, NetherNet::ConnectResponse, NetherNet::ConnectError, NetherNet::CandidateAdd>;
// clang-format on
using SendResult = NetherNet::ErrorOr<void, NetherNet::ESessionError>;

/// The signal as "<TYPE> <connection id> <payload>"; empty when the text does not look like what the layout guess
/// says it is (then the layout is wrong: see SignalLayout.h).
std::optional<std::string> serialize(Signal const& signal) {
    switch (signal.index()) {
    case 0: {
        auto const& s = reinterpret_cast<SignalWithText const&>(std::get<0>(signal));
        if (!layout::plausibleText(s.text)) return std::nullopt;
        return fmt::format("CONNECTREQUEST {} {}", s.connectionId, s.text);
    }
    case 1: {
        auto const& s = reinterpret_cast<SignalWithText const&>(std::get<1>(signal));
        if (!layout::plausibleText(s.text) || !s.text.starts_with("v=0")) return std::nullopt;
        return fmt::format("CONNECTRESPONSE {} {}", s.connectionId, s.text);
    }
    case 2: {
        auto const& s = reinterpret_cast<SignalWithCode const&>(std::get<2>(signal));
        return fmt::format("CONNECTERROR {} {}", s.connectionId, s.code);
    }
    case 3: {
        auto const& s = reinterpret_cast<SignalWithText const&>(std::get<3>(signal));
        // Empty: the end of the candidates.
        if (!layout::plausibleText(s.text) || !(s.text.empty() || s.text.starts_with("candidate:"))) {
            return std::nullopt;
        }
        return fmt::format("CANDIDATEADD {} {}", s.connectionId, s.text);
    }
    default:
        return std::nullopt;
    }
}

// Whether the mod's guesses at BDS's NetherNet layouts hold. prime() checks them with a probe at startup; the hooks
// check plausibility at run time. Mismatch is final: the mod stops touching the transport and the server is not
// published.
enum class Layout { Unchecked, Verified, Mismatch };
std::atomic<Layout> layoutState{Layout::Unchecked};
std::atomic<bool>   reportedMismatch{false};
// False when LeviLamina could not install a required hook; set once by install().
std::atomic<bool> hooksInstalled{true};
std::atomic<bool> reportedMissingHooks{false};
ll::io::Logger*   logger = nullptr;

// The LeviLamina version that runs now, without build metadata. Only for error messages.
std::string runningLeviLaminaVersion() {
    try {
        auto version = ll::getLoaderVersion();
        version.build.reset();
        return version.to_string();
    } catch (...) {
        return "(unknown version)";
    }
}

void reportMismatch() {
    layoutState.store(Layout::Mismatch);
    if (!reportedMismatch.exchange(true) && logger) {
        logger->error(
            "This build of XboxLiveOnBDS does not match the NetherNet internals of this server (LeviLamina {}; it is "
            "built for LeviLamina {}), so friends cannot join through Xbox Live and the server is not shown in the "
            "Friends tab. Use the mod release made for your LeviLamina version.",
            runningLeviLaminaVersion(),
            LEVILAMINA_BUILD_VERSION
        );
    }
}

// The probe prime() sends: while `probeId` is not 0, the hooks look for the signals BDS gives that id, and note
// whether they read as the layout guesses say. Guarded by `mutex`.
std::atomic<std::uint64_t> probeId{0};
struct ProbeResult {
    bool          receiveSeen = false, receiveOk = false;
    bool          sendSeen = false, sendOk = false;
    std::uint64_t connectionId = 0;
};
ProbeResult             probeResult;
std::condition_variable probeSeen;

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
    if (auto probe = probeId.load(); probe != 0) {
        try {
            // toString() is BDS's own code: it finds the probe without our layout.
            if (from.toString() == std::to_string(probe)) {
                auto connection = connectRequestId(message);
                bool ok         = p2pValue(from) == probe && connection;
                {
                    std::lock_guard lock(mutex);
                    probeResult.receiveSeen  = true;
                    probeResult.receiveOk    = ok;
                    probeResult.connectionId = connection.value_or(0);
                }
                probeSeen.notify_all();
            }
        } catch (...) {}
    }
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
    if (auto probe = probeId.load(); probe != 0) {
        try {
            if (networkIDTo.toString() == std::to_string(probe)) {
                bool ok = p2pValue(networkIDTo) == probe && signal.index() == 2;
                if (ok) {
                    auto const& s = reinterpret_cast<SignalWithCode const&>(std::get<2>(signal));
                    {
                        std::lock_guard lock(mutex);
                        ok = s.connectionId == probeResult.connectionId && s.code >= 1 && s.code <= 1000;
                    }
                }
                {
                    std::lock_guard lock(mutex);
                    probeResult.sendSeen = true;
                    probeResult.sendOk   = ok;
                }
                probeSeen.notify_all();
                return origin(networkIDTo, signal, preference); // The probe is no peer's.
            }
        } catch (...) {}
    }
    if (layoutState.load() == Layout::Mismatch) return origin(networkIDTo, signal, preference);
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
            std::optional<std::string> text;
            try {
                text = serialize(signal);
            } catch (...) {}
            if (text) target(*to, std::move(*text));
            else reportMismatch();
        }
    }
    return origin(networkIDTo, signal, preference);
}

std::vector<RelayServer> relayServers; // guarded by mutex
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
    if (layoutState.load() == Layout::Mismatch) {
        origin(config, offer, std::move(onComplete));
        return;
    }
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
                "Xbox Live sent no relay servers for a friend's join; if the join does not complete, the friend can "
                "try again"
            );
            logger->debug("The join uses the ICE servers of the server");
            logger->debug("No relay servers for connection {}", util::Redactor::instance().id(connectionId));
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

namespace {
// Installs the hooks once. hook() returns 0 on success. A hook that fails (LeviLamina lacks the function) is left out;
// the ones that did install are harmless, because prime() and deliver() stop any use when one is missing.
struct HookGuard {
    bool capture = false, forward = false, relay = false;

    HookGuard()
    : capture(CaptureTransportHook::hook() == 0),
      forward(ForwardToPeersHook::hook() == 0),
      relay(AddRelayServersHook::hook() == 0) {
        // Keeps the hooks' reference counts as HookRegistrar did (this also uses the member, which is otherwise
        // unused).
        ++CaptureTransportHook::_AutoHookCount;
        ++ForwardToPeersHook::_AutoHookCount;
        ++AddRelayServersHook::_AutoHookCount;
        hooksInstalled.store(capture && forward && relay);
    }
    ~HookGuard() {
        --CaptureTransportHook::_AutoHookCount;
        --ForwardToPeersHook::_AutoHookCount;
        --AddRelayServersHook::_AutoHookCount;
        if (capture) CaptureTransportHook::unhook();
        if (forward) ForwardToPeersHook::unhook();
        if (relay) AddRelayServersHook::unhook();
    }
    HookGuard(HookGuard const&)            = delete;
    HookGuard& operator=(HookGuard const&) = delete;
};
} // namespace

void install(ll::io::Logger& log) {
    logger = &log;
    static HookGuard guard;
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
    if (!hooksInstalled.load()) {
        if (!reportedMissingHooks.exchange(true) && logger) {
            logger->error(
                "LeviLamina {} lacks NetherNet functions this mod needs (it is built for LeviLamina {}), so friends "
                "cannot join and the server is not shown in the Friends tab. Use the mod release made for your "
                "LeviLamina version.",
                runningLeviLaminaVersion(),
                LEVILAMINA_BUILD_VERSION
            );
        }
        return false;
    }
    if (layoutState.load() == Layout::Verified) return true;
    if (layoutState.load() == Layout::Mismatch) return false;
    // A NetherNet id no friend has (theirs are random 64-bit values); BDS's reply to it goes nowhere.
    std::uint64_t id = (static_cast<std::uint64_t>(std::random_device{}()) << 32 | std::random_device{}()) | 1;
    {
        std::lock_guard lock(mutex);
        probeResult = {};
    }
    probeId.store(id);
    auto res = net::request(
        "POST",
        fmt::format("http://127.0.0.1:{}/v1/join/{}", port, id),
        {
            {"Content-Type", "application/sdp"}
    },
        "v=0"
    );
    // The hooks ran during the request; the wait covers a transport that answers on another thread.
    ProbeResult seen;
    {
        std::unique_lock lock(mutex);
        probeSeen.wait_for(lock, std::chrono::seconds(1), [] {
            return probeResult.receiveSeen && probeResult.sendSeen;
        });
        seen = probeResult;
    }
    probeId.store(0);
    if (seen.receiveSeen && seen.sendSeen && seen.receiveOk && seen.sendOk) {
        auto expected = Layout::Unchecked;
        if (layoutState.compare_exchange_strong(expected, Layout::Verified) && logger) {
            logger->debug("Checked the server's NetherNet layout");
        }
    } else if ((seen.receiveSeen && !seen.receiveOk) || (seen.sendSeen && !seen.sendOk)) {
        reportMismatch();
    } else if (logger) {
        logger->warn(
            "Could not check the server's NetherNet transport ({}); if the first friend to join does not get in, that "
            "friend "
            "can try again",
            res.plainCause()
        );
        logger->debug("NetherNet transport check: {}", util::Redactor::instance().text(res.describe()));
    }
    return layoutState.load() != Layout::Mismatch;
}

bool deliver(std::uint64_t from, std::string_view signal) {
    if (!hooksInstalled.load()) return true;
    // Not the layout we guessed: touch nothing, and drop the signal rather than fall back to HTTP signaling.
    if (layoutState.load() == Layout::Mismatch) return true;
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
