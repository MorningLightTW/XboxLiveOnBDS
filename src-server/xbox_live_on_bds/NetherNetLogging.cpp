#include "NetherNetLogging.h"

#include "ll/api/memory/Hook.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <variant>

#include <fmt/format.h>

#include "ll/api/io/Logger.h"

#include "mc/deps/nether_net/NetworkID.h"
#include "mc/deps/nether_net/ServerNegotiator.h"
#include "mc/deps/nether_net/SimpleNetworkInterfaceImpl.h"
#include "mc/deps/nether_net/signaling/CandidateAdd.h"
#include "mc/deps/nether_net/signaling/ConnectError.h"
#include "mc/deps/nether_net/signaling/ConnectRequest.h"
#include "mc/deps/nether_net/signaling/ConnectResponse.h"

namespace xbox_live_on_bds {

namespace {

ll::io::Logger* traceLogger = nullptr;

// Full signals of each join, one file per connection id (debugging only: addresses and identity tokens).
std::filesystem::path   dumpDir;
std::mutex              dumpMutex;
std::set<std::uint64_t> dumpedJoins; // guarded by dumpMutex
constexpr std::size_t   MaxDumpedJoins = 64;

/// Appends "<TYPE> <connection id> <payload>" to that connection's file.
void dumpSignal(std::string_view direction, std::string_view peer, std::string_view message) {
    if (dumpDir.empty()) return;
    auto first = message.find(' ');
    if (first == std::string_view::npos) return;
    auto          idText = message.substr(first + 1, message.find(' ', first + 1) - first - 1);
    std::uint64_t id{};
    if (std::from_chars(idText.data(), idText.data() + idText.size(), id).ec != std::errc{}) return;
    std::lock_guard lock(dumpMutex);
    if (!dumpedJoins.contains(id)) {
        if (dumpedJoins.size() >= MaxDumpedJoins) return;
        dumpedJoins.insert(id);
    }
    std::error_code ec;
    std::filesystem::create_directories(dumpDir, ec);
    std::ofstream(dumpDir / fmt::format("join-{}.txt", id), std::ios::binary | std::ios::app)
        << direction << ' ' << peer << '\n'
        << message << "\n\n";
}

// The signal classes are laid out as { uint64 connection id; payload } (see TransportBridge.cpp).
struct SignalWithText {
    std::uint64_t connectionId;
    std::string   text;
};
struct SignalWithCode {
    std::uint64_t connectionId;
    int           code;
};

std::string_view channelName(NetherNet::SignalingChannelId channel) {
    return channel == NetherNet::SignalingChannelId::Lan ? "LAN" : "WebSocket";
}

// What BDS's NetherNet transport receives on its signaling channels...
LL_TYPE_INSTANCE_HOOK(
    TraceReceiveHook,
    ll::memory::HookPriority::Normal,
    NetherNet::SimpleNetworkInterfaceImpl,
    &NetherNet::SimpleNetworkInterfaceImpl::ReceiveFromSignalingChannel,
    void,
    NetherNet::NetworkID          from,
    std::string_view              message,
    NetherNet::SignalingChannelId sourceChannel
) {
    if (traceLogger) {
        auto head = message.substr(0, std::min<size_t>(message.find(' ', message.find(' ') + 1), 60));
        traceLogger->info(
            "[NetherNet] <- {} from {}: {} ({} bytes)",
            channelName(sourceChannel),
            from.toString(),
            head,
            message.size()
        );
        dumpSignal(fmt::format("<- {} from", channelName(sourceChannel)), from.toString(), message);
    }
    origin(std::move(from), message, sourceChannel);
}

// clang-format off
using Signal =
    std::variant<NetherNet::ConnectRequest, NetherNet::ConnectResponse, NetherNet::ConnectError, NetherNet::CandidateAdd>;
// clang-format on
using SendResult = NetherNet::ErrorOr<void, NetherNet::ESessionError>;

// ...and what it sends back, on which channel.
LL_TYPE_INSTANCE_HOOK(
    TraceSendHook,
    ll::memory::HookPriority::Normal,
    NetherNet::SimpleNetworkInterfaceImpl,
    &NetherNet::SimpleNetworkInterfaceImpl::SendToSignalingChannel,
    SendResult,
    NetherNet::NetworkID                         networkIDTo,
    Signal const&                                signal,
    std::optional<NetherNet::SignalingChannelId> preference
) {
    static constexpr std::string_view kinds[] = {"CONNECTREQUEST", "CONNECTRESPONSE", "CONNECTERROR", "CANDIDATEADD"};
    std::string                       to      = networkIDTo.toString();
    if (traceLogger) {
        traceLogger->info(
            "[NetherNet] -> {} to {} (preferred channel: {})",
            kinds[signal.index()],
            to,
            preference ? channelName(*preference) : "none"
        );
        std::string text;
        if (signal.index() == 2) {
            auto const& s = reinterpret_cast<SignalWithCode const&>(std::get<2>(signal));
            text          = fmt::format("{} {} {}", kinds[2], s.connectionId, s.code);
        } else {
            std::visit(
                [&](auto const& alternative) {
                    auto const& s = reinterpret_cast<SignalWithText const&>(alternative);
                    text          = fmt::format("{} {} {}", kinds[signal.index()], s.connectionId, s.text);
                },
                signal
            );
        }
        dumpSignal("-> to", to, text);
    }
    // The return type is opaque here (smaller than the real object), so it is returned straight from origin and
    // never copied; logging therefore happens before the call (same reason as ForwardToPeersHook).
    return origin(std::move(networkIDTo), signal, preference);
}

using PeerConnectionResult =
    Bedrock::Result<webrtc::scoped_refptr<webrtc::PeerConnectionInterface>, NetherNet::ESessionError>;
using AnswerCallback = brstd::move_only_function<void(PeerConnectionResult)>;

// Whether BDS starts building an answer for an offer, and how that ends.
LL_TYPE_INSTANCE_HOOK(
    TraceCreateAnswerHook,
    ll::memory::HookPriority::Lowest, // After LeviLamina's STUN patch and the relay servers have had their say.
    NetherNet::ServerNegotiator,
    &NetherNet::ServerNegotiator::$createAnswer,
    void,
    webrtc::PeerConnectionInterface::RTCConfiguration const& config,
    NetherNet::ConnectRequest const&                         offer,
    AnswerCallback&&                                         onComplete
) {
    // Only note the start: wrapping onComplete in a new move_only_function crashes BDS (CFG) when it calls it.
    // The outcome shows as the CONNECTRESPONSE / CONNECTERROR the transport sends next.
    if (traceLogger) {
        std::string urls; // No credentials: just where each server is.
        for (auto const& server : config.servers.get()) {
            if (!server.uri.empty()) urls += fmt::format(" {}", server.uri);
            for (auto const& url : server.urls) urls += fmt::format(" {}", url);
        }
        traceLogger->info("[NetherNet] createAnswer started ({} ICE server(s):{})", config.servers->size(), urls);
    }
    origin(config, offer, std::move(onComplete));
}

} // namespace

void enableSignalingTrace(ll::io::Logger& logger, std::filesystem::path dir) {
    traceLogger = &logger;
    dumpDir     = std::move(dir);
    static ll::memory::HookRegistrar<TraceReceiveHook, TraceSendHook, TraceCreateAnswerHook> registrar;
}

} // namespace xbox_live_on_bds
