#pragma once

#include <cstdint>
#include <string>

#include "mc/deps/nether_net/signaling/CandidateAdd.h"
#include "mc/deps/nether_net/signaling/ConnectError.h"
#include "mc/deps/nether_net/signaling/ConnectRequest.h"
#include "mc/deps/nether_net/signaling/ConnectResponse.h"

/// Internal. The signal classes of NetherNet are opaque in the headers. These are guesses at their layout:
/// { uint64 connection id; payload }, the payload being the SDP / candidate string, or the int error code for
/// ConnectError. transport_bridge::prime() checks the guesses against a probe at startup, and the hooks check
/// plausibleText() at run time.
namespace xbox_live_on_bds::bds::layout {

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

/// Whether `text` looks like a real std::string (MSVC layout): size within capacity, capacity at least the SSO
/// buffer, and a size no signal reaches.
inline bool plausibleText(std::string const& text) {
    return text.size() <= text.capacity() && text.capacity() >= 15 && text.size() <= 64 * 1024;
}

} // namespace xbox_live_on_bds::bds::layout
