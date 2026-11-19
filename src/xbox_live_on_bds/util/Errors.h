#pragma once

#include <exception>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace xbox_live_on_bds::util {

/// The text of an exception for a warn or error log line. The what() of a JSON parse error quotes the bytes it read
/// last, and these can be part of a token or of remote data, so only the position is shown. Does not throw.
inline std::string describeException(std::exception const& e) noexcept {
    try {
        if (auto const* parse = dynamic_cast<nlohmann::json::parse_error const*>(&e)) {
            return fmt::format("invalid JSON at byte {}", parse->byte);
        }
        return e.what();
    } catch (...) {
        return "error";
    }
}

} // namespace xbox_live_on_bds::util
