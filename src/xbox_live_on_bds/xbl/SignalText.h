#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace xbox_live_on_bds::xbl {

/// A NetherNet signal as it travels over signaling: "<TYPE> <connection id> <payload>".
struct SignalText {
    std::string_view type;
    std::string_view connectionId; // Decimal digits only: safe to log and to put in a URL.
    std::string_view payload;
};

/// Splits a signal a remote player sent. Empty unless it has all three parts and the connection id is a decimal
/// 64-bit number, as BDS and the game write it.
std::optional<SignalText> parseSignal(std::string_view raw);

/// The NetherNet id a remote player claims, from its text. Empty unless it is a decimal 64-bit number other than 0
/// and nothing else.
std::optional<std::uint64_t> parseNetherNetId(std::string_view text);

} // namespace xbox_live_on_bds::xbl
