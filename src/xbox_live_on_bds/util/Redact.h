#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace xbox_live_on_bds::util {

/// Replaces private values (addresses, ids, names, tokens) in debug output by stable aliases, so that a debug log
/// or a debug file can go into a public issue. The same value gets the same alias while it stays in the table: ip1,
/// peer2, player3. A full table (1024 values per kind by default) is cleared and its numbering goes on, so aliases
/// stay unique; a value seen again after a clear gets a new alias. When redaction is off, every function returns
/// its input.
/// Conservative by design: when unsure, it redacts. Thread-safe; no function throws.
class Redactor {
public:
    explicit Redactor(bool enabled = true, std::size_t maxPerKind = 1024);

    /// The one the mod uses. Redacting until Mod switches it off (raw debug mode).
    static Redactor& instance();

    void setEnabled(bool enabled);
    bool enabled() const;

    /// A NetherNet id or a connection id.
    std::string id(std::string_view value);
    std::string id(std::uint64_t value);
    /// A gamertag, a player name or an XUID.
    std::string player(std::string_view value);
    /// One address, with or without a port.
    std::string ip(std::string_view value);
    /// SDP, or trickled candidate text: addresses, ufrag, password and fingerprint hidden.
    std::string sdp(std::string_view value);
    /// A NetherNet signal, "<TYPE> <connection id> <payload>".
    std::string signal(std::string_view value);
    /// Free text, a remote payload or an HTTP body.
    std::string text(std::string_view value);

private:
    enum class Kind : std::size_t { Ip, Peer, Player, Mdns };

    std::string alias(Kind kind, std::string_view value);
    std::string addressAlias(std::string_view address);
    std::string textImpl(std::string_view value);
    std::string sdpImpl(std::string_view value);
    std::string signalImpl(std::string_view value);
    std::string sdpLine(std::string_view line);
    std::string candidateFields(std::string_view prefix, std::string_view fields, bool& ok);

    std::atomic<bool>                                 mEnabled;
    std::size_t                                       mMaxPerKind;
    std::mutex                                        mMutex;
    std::array<std::map<std::string, std::string>, 4> mAliases; // guarded by mMutex
    std::array<std::size_t, 4> mCounters{};                     // Never reset: aliases stay unique. Guarded by mMutex.
};

} // namespace xbox_live_on_bds::util
