#include "xbox_live_on_bds/util/Redact.h"

#include <algorithm>
#include <vector>

namespace xbox_live_on_bds::util {

namespace {

constexpr std::array<std::string_view, 4> KindPrefixes = {"ip", "peer", "player", "mdns"};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool isAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
// base64, base64url, hex, "=" padding: what a token is made of.
bool isTokenChar(char c) { return isAlnum(c) || c == '+' || c == '/' || c == '=' || c == '_' || c == '-'; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string charsMark(std::size_t n) { return "<" + std::to_string(n) + " chars>"; }
std::string tokenMark(std::size_t n) { return "<token " + std::to_string(n) + " chars>"; }
std::string failed(std::string_view value) { return "<redacted " + std::to_string(value.size()) + " bytes>"; }

/// Length of the dotted IPv4 literal at the start of s, or 0.
std::size_t ipv4Length(std::string_view s) {
    std::size_t pos = 0;
    for (int group = 0; group < 4; ++group) {
        if (group > 0) {
            if (pos >= s.size() || s[pos] != '.') return 0;
            ++pos;
        }
        std::size_t start = pos;
        int         value = 0;
        while (pos < s.size() && isDigit(s[pos]) && pos - start < 3) value = value * 10 + (s[pos++] - '0');
        if (pos == start || value > 255) return 0;
    }
    if (pos < s.size() && isDigit(s[pos])) return 0;
    if (pos + 1 < s.size() && s[pos] == '.' && isDigit(s[pos + 1])) return 0; // A version number, not an address.
    return pos;
}

bool validIpv4(std::string_view s) { return ipv4Length(s) == s.size(); }

bool validIpv6(std::string_view s) {
    if (s.size() < 2 || s.size() > 45) return false;
    std::size_t pos    = 0;
    int         groups = 0;
    bool        gap    = false;
    if (s.starts_with("::")) {
        gap = true;
        pos = 2;
        if (pos == s.size()) return true;
    } else if (s[0] == ':') {
        return false;
    }
    while (pos < s.size()) {
        std::size_t end = pos;
        while (end < s.size() && isHex(s[end])) ++end;
        if (end < s.size() && s[end] == '.') { // IPv4 tail: two groups, and the end.
            if (ipv4Length(s.substr(pos)) != s.size() - pos) return false;
            groups += 2;
            break;
        }
        if (end == pos || end - pos > 4) return false;
        ++groups;
        pos = end;
        if (pos == s.size()) break;
        if (s[pos] != ':') return false;
        ++pos;
        if (pos == s.size()) return false;
        if (s[pos] == ':') {
            if (gap) return false;
            gap = true;
            ++pos;
        }
    }
    return gap ? groups <= 7 : groups == 8;
}

bool isIp(std::string_view s) { return validIpv4(s) || (s.find(':') != std::string_view::npos && validIpv6(s)); }

/// "v4,private" and the like. Private: RFC 1918, loopback, link-local, CGNAT, IPv6 ULA / link-local.
std::string classify(std::string_view address) {
    if (validIpv4(address)) {
        int octet[4]{};
        int index = 0;
        for (char c : address) {
            if (c == '.') ++index;
            else octet[index] = octet[index] * 10 + (c - '0');
        }
        bool priv = octet[0] == 10 || octet[0] == 127 || (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31)
                 || (octet[0] == 192 && octet[1] == 168) || (octet[0] == 169 && octet[1] == 254)
                 || (octet[0] == 100 && octet[1] >= 64 && octet[1] <= 127);
        return priv ? "v4,private" : "v4,public";
    }
    std::string a    = lower(address);
    unsigned    head = 0;
    for (std::size_t i = 0; i < a.size() && a[i] != ':'; ++i) {
        head = head * 16 + static_cast<unsigned>(isDigit(a[i]) ? a[i] - '0' : a[i] - 'a' + 10);
        if (head > 0xffff) break;
    }
    bool priv = a == "::1" || (head & 0xfe00) == 0xfc00 || (head & 0xffc0) == 0xfe80;
    return priv ? "v6,private" : "v6,public";
}

bool endsWithNoCase(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == suffix;
}

std::vector<std::string_view> splitSpaces(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t                   pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && s[pos] == ' ') ++pos;
        std::size_t end = s.find(' ', pos);
        if (end == std::string_view::npos) end = s.size();
        if (end > pos) out.push_back(s.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

bool allDigits(std::string_view s) { return !s.empty() && std::ranges::all_of(s, isDigit); }

} // namespace

Redactor::Redactor(bool enabled, std::size_t maxPerKind) : mEnabled(enabled), mMaxPerKind(maxPerKind) {}

Redactor& Redactor::instance() {
    static Redactor redactor;
    return redactor;
}

void Redactor::setEnabled(bool enabled) { mEnabled = enabled; }

bool Redactor::enabled() const { return mEnabled; }

std::string Redactor::alias(Kind kind, std::string_view value) {
    auto            index  = static_cast<std::size_t>(kind);
    std::string     prefix = std::string(KindPrefixes[index]);
    std::lock_guard lock(mMutex);
    auto&           table = mAliases[index];
    std::string     key(value);
    if (auto it = table.find(key); it != table.end()) return it->second;
    if (table.size() >= mMaxPerKind) table.clear(); // Expiry: old values get a new alias when seen again.
    return table.emplace(std::move(key), prefix + std::to_string(++mCounters[index])).first->second;
}

// ip3(v4,private), or mdns1 for a name that only the sender's network resolves.
std::string Redactor::addressAlias(std::string_view address) {
    if (isIp(address)) return alias(Kind::Ip, lower(address)) + "(" + classify(address) + ")";
    if (endsWithNoCase(address, ".local")) return alias(Kind::Mdns, lower(address));
    return textImpl(address);
}

std::string Redactor::id(std::string_view value) {
    if (!mEnabled) return std::string(value);
    try {
        return alias(Kind::Peer, value);
    } catch (...) {
        return failed(value);
    }
}

std::string Redactor::id(std::uint64_t value) { return id(std::to_string(value)); }

std::string Redactor::player(std::string_view value) {
    if (!mEnabled) return std::string(value);
    try {
        return alias(Kind::Player, value);
    } catch (...) {
        return failed(value);
    }
}

std::string Redactor::ip(std::string_view value) {
    if (!mEnabled) return std::string(value);
    try {
        auto out = textImpl(value);
        if (out == value) out = alias(Kind::Ip, lower(value)); // Not recognized as an address: hide it anyway.
        return out;
    } catch (...) {
        return failed(value);
    }
}

std::string Redactor::text(std::string_view value) {
    if (!mEnabled) return std::string(value);
    try {
        return textImpl(value);
    } catch (...) {
        return failed(value);
    }
}

std::string Redactor::sdp(std::string_view value) {
    if (!mEnabled) return std::string(value);
    try {
        return sdpImpl(value);
    } catch (...) {
        return failed(value);
    }
}

std::string Redactor::signal(std::string_view value) {
    if (!mEnabled) return std::string(value);
    try {
        return signalImpl(value);
    } catch (...) {
        return failed(value);
    }
}

std::string Redactor::textImpl(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    std::size_t i = 0;
    std::size_t n = value.size();
    while (i < n) {
        char c = value[i];
        if (value.substr(i).starts_with("XBL3.0 x=")) {
            std::size_t j = i + 9; // Past "XBL3.0 x=".
            while (j < n && !isSpace(value[j]) && value[j] != '"' && value[j] != '\'') ++j;
            out += "<token>";
            i    = j;
            continue;
        }
        if ((i == 0 || !isTokenChar(value[i - 1])) && isTokenChar(c)) {
            std::size_t j = i;
            if (value.substr(i).starts_with("eyJ")) { // A JWT: base64url segments with dots.
                while (j < n && (isTokenChar(value[j]) || value[j] == '.')) ++j;
                out += tokenMark(j - i);
                i    = j;
                continue;
            }
            std::size_t alnum = 0;
            while (j < n && isTokenChar(value[j])) {
                if (isAlnum(value[j])) ++alnum;
                ++j;
            }
            if (j - i >= 32 && alnum >= 16) {
                out += tokenMark(j - i);
                i    = j;
                continue;
            }
        }
        bool wordStart = i == 0 || (!isAlnum(value[i - 1]) && value[i - 1] != '.');
        if (isDigit(c) && wordStart) {
            std::size_t digits = 0;
            while (i + digits < n && isDigit(value[i + digits])) ++digits;
            bool tail = i + digits < n && isAlnum(value[i + digits]);
            if (digits == 16 && value.substr(i, 4) == "2533" && !tail) {
                out += alias(Kind::Player, value.substr(i, digits));
                i   += digits;
                continue;
            }
            if (auto len = ipv4Length(value.substr(i))) {
                out += alias(Kind::Ip, value.substr(i, len));
                i   += len;
                continue;
            }
        }
        if ((isHex(c) || c == ':') && wordStart && (i == 0 || value[i - 1] != ':')) {
            std::size_t j = i;
            while (j < n && (isHex(value[j]) || value[j] == ':' || value[j] == '.')) ++j;
            auto candidate = value.substr(i, std::min<std::size_t>(j - i, 45)); // Longest IPv6 literal.
            while (!candidate.empty() && !validIpv6(candidate)) candidate.remove_suffix(1);
            if (!candidate.empty() && std::ranges::count(candidate, ':') >= 2
                && std::ranges::any_of(candidate, isHex)) {
                out += alias(Kind::Ip, lower(candidate));
                i   += candidate.size();
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// The fields of a candidate after "candidate:", without the foundation.
std::string Redactor::candidateFields(std::string_view prefix, std::string_view fields, bool& ok) {
    auto tokens = splitSpaces(fields);
    ok          = tokens.size() >= 8 && tokens[6] == "typ" && allDigits(tokens[1]) && allDigits(tokens[3])
               && allDigits(tokens[5]);
    if (!ok) return {};
    std::string out(prefix);
    out += "candidate:_ ";
    out += textImpl(tokens[1]) + ' ' + textImpl(tokens[2]) + ' ' + textImpl(tokens[3]) + ' ' + addressAlias(tokens[4])
         + ' ' + std::string(tokens[5]) + " typ " + textImpl(tokens[7]);
    for (std::size_t i = 8; i < tokens.size(); ++i) {
        out += ' ';
        if (tokens[i] == "raddr" && i + 1 < tokens.size()) {
            out += "raddr " + addressAlias(tokens[i + 1]);
            ++i;
        } else if (tokens[i] == "ufrag" && i + 1 < tokens.size()) {
            out += "ufrag " + charsMark(tokens[i + 1].size());
            ++i;
        } else {
            out += textImpl(tokens[i]);
        }
    }
    return out;
}

std::string Redactor::sdpLine(std::string_view line) {
    std::string_view prefix;
    std::string_view rest = line;
    if (rest.starts_with("a=")) {
        prefix = rest.substr(0, 2);
        rest.remove_prefix(2);
    }
    if (rest.starts_with("candidate:")) {
        bool ok     = false;
        auto result = candidateFields(prefix, rest.substr(10), ok);
        return ok ? result : textImpl(line);
    }
    for (std::string_view key : {"a=ice-ufrag:", "a=ice-pwd:", "a=fingerprint:"}) {
        if (line.starts_with(key)) return std::string(key) + charsMark(line.size() - key.size());
    }
    for (std::string_view key : {"c=IN IP4 ", "c=IN IP6 "}) {
        if (line.starts_with(key)) {
            auto addr = line.substr(key.size());
            auto end  = addr.find(' ');
            auto tail = end == std::string_view::npos ? std::string_view{} : addr.substr(end);
            return std::string(key) + addressAlias(addr.substr(0, end)) + textImpl(tail);
        }
    }
    return textImpl(line);
}

std::string Redactor::sdpImpl(std::string_view value) {
    std::string out;
    std::size_t pos = 0;
    while (pos < value.size()) {
        std::size_t      end  = value.find('\n', pos);
        bool             last = end == std::string_view::npos;
        auto             line = value.substr(pos, last ? std::string_view::npos : end - pos);
        std::string_view eol  = last ? std::string_view{} : std::string_view("\n");
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
            eol = last ? "\r" : "\r\n";
        }
        out += sdpLine(line);
        out += eol;
        if (last) break;
        pos = end + 1;
    }
    return out;
}

std::string Redactor::signalImpl(std::string_view value) {
    auto first = value.find(' ');
    if (first == std::string_view::npos || first == 0) return textImpl(value);
    auto type = value.substr(0, first);
    if (!std::ranges::all_of(type, [](char c) { return c >= 'A' && c <= 'Z'; })) return textImpl(value);
    auto second = value.find(' ', first + 1);
    auto idText =
        value.substr(first + 1, second == std::string_view::npos ? std::string_view::npos : second - first - 1);
    if (!allDigits(idText)) return textImpl(value);
    auto payload = second == std::string_view::npos ? std::string_view{} : value.substr(second + 1);

    std::string out(type);
    out += ' ' + alias(Kind::Peer, idText) + ' ';
    if (type == "CONNECTERROR") out += allDigits(payload) ? std::string(payload) : textImpl(payload);
    else if (type == "CONNECTREQUEST" || type == "CONNECTRESPONSE" || type == "CANDIDATEADD") out += sdpImpl(payload);
    else out += textImpl(payload);
    return out;
}

} // namespace xbox_live_on_bds::util
