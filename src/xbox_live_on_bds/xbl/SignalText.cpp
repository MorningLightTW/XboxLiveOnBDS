#include "xbox_live_on_bds/xbl/SignalText.h"

#include <charconv>

namespace xbox_live_on_bds::xbl {

namespace {

std::optional<std::uint64_t> parseDecimal(std::string_view text) {
    std::uint64_t value{};
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

std::optional<SignalText> parseSignal(std::string_view raw) {
    auto first = raw.find(' ');
    if (first == std::string_view::npos || first == 0) return std::nullopt;
    auto second = raw.find(' ', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    SignalText signal{raw.substr(0, first), raw.substr(first + 1, second - first - 1), raw.substr(second + 1)};
    if (!parseDecimal(signal.connectionId)) return std::nullopt;
    return signal;
}

std::optional<std::uint64_t> parseNetherNetId(std::string_view text) {
    auto id = parseDecimal(text);
    if (!id || *id == 0) return std::nullopt;
    return id;
}

} // namespace xbox_live_on_bds::xbl
