#include "xbox_live_on_bds/util/Encoding.h"

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>

#include <fmt/format.h>

#include <windows.h>

#include <bcrypt.h>

namespace xbox_live_on_bds::util {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int          length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

namespace {
constexpr std::string_view Base64Alphabet    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view Base64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
} // namespace

std::string base64Encode(std::string_view bytes, bool url) {
    auto const& alphabet = url ? Base64UrlAlphabet : Base64Alphabet;
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        uint32_t n  = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
        out        += alphabet[(n >> 18) & 63];
        out        += alphabet[(n >> 12) & 63];
        out        += alphabet[(n >> 6) & 63];
        out        += alphabet[n & 63];
    }
    if (size_t rest = bytes.size() - i; rest > 0) {
        uint32_t n = uint8_t(bytes[i]) << 16;
        if (rest == 2) n |= uint8_t(bytes[i + 1]) << 8;
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        if (rest == 2) out += alphabet[(n >> 6) & 63];
        if (!url) out.append(rest == 1 ? "==" : "=");
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    std::array<int, 256> value{};
    value.fill(-1);
    for (size_t i = 0; i < 64; ++i) {
        value[uint8_t(Base64Alphabet[i])]    = int(i);
        value[uint8_t(Base64UrlAlphabet[i])] = int(i);
    }
    std::string out;
    uint32_t    buffer = 0;
    int         bits   = 0;
    for (char c : text) {
        if (c == '=') break;
        int v = value[uint8_t(c)];
        if (v < 0) return std::nullopt;
        buffer  = (buffer << 6) | uint32_t(v);
        bits   += 6;
        if (bits >= 8) {
            bits -= 8;
            out  += char((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

std::string formEncode(std::vector<std::pair<std::string, std::string>> const& fields) {
    auto escape = [](std::string_view s) {
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
            else out += fmt::format("%{:02X}", c);
        }
        return out;
    };
    std::string body;
    for (auto const& [key, value] : fields) {
        if (!body.empty()) body += '&';
        body += escape(key) + '=' + escape(value);
    }
    return body;
}

std::string randomUuid() {
    std::array<uint8_t, 16> b{};
    BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);
    return fmt::format(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        b[0],
        b[1],
        b[2],
        b[3],
        b[4],
        b[5],
        b[6],
        b[7],
        b[8],
        b[9],
        b[10],
        b[11],
        b[12],
        b[13],
        b[14],
        b[15]
    );
}

std::int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t parseIsoTime(std::string_view text) {
    // YYYY-MM-DDTHH:MM:SS, then an optional fraction and the zone, which is always Z for Xbox Live.
    std::tm     tm{};
    int*        fields[]     = {&tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec};
    char        separators[] = {'-', '-', 'T', ':', ':'};
    char const* p            = text.data();
    char const* end          = text.data() + text.size();
    for (int i = 0; i < 6; ++i) {
        auto [next, ec] = std::from_chars(p, end, *fields[i]);
        if (ec != std::errc{}) return 0;
        p = next;
        if (i < 5) {
            if (p == end || *p != separators[i]) return 0;
            ++p;
        }
    }
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    auto t      = _mkgmtime64(&tm);
    return t < 0 ? 0 : t;
}

} // namespace xbox_live_on_bds::util
