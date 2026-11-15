#pragma once

#include <filesystem>

namespace ll::io {
class Logger;
}

namespace xbox_live_on_bds::bds {

/// Debugging aid: log every signal BDS's NetherNet transport receives and sends, and each answer it starts
/// building with its ICE servers. With `dumpDir` set, each join's signals in full (offer, answer, candidates) are
/// appended to <dumpDir>/join-<connection id>.txt: they hold players' addresses and identity tokens.
/// Must run in load(), before the server starts.
void enableSignalingTrace(ll::io::Logger& logger, std::filesystem::path dumpDir = {});

} // namespace xbox_live_on_bds::bds
