#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xbox_live_on_bds::util {

std::wstring widen(std::string_view utf8);

/// Standard base64 with padding, or base64url without padding (JWK coordinates).
std::string                base64Encode(std::string_view bytes, bool url = false);
std::optional<std::string> base64Decode(std::string_view text);

/// application/x-www-form-urlencoded body.
std::string formEncode(std::vector<std::pair<std::string, std::string>> const& fields);

/// Random version 4 UUID, lowercase, without braces.
std::string randomUuid();

/// Seconds since the Unix epoch.
std::int64_t unixNow();

/// Parses an ISO 8601 UTC time such as "2026-11-27T05:23:18.4637329Z" into Unix seconds (0 on failure).
std::int64_t parseIsoTime(std::string_view text);

} // namespace xbox_live_on_bds::util
