#include "xbox_live_on_bds/ice/Stun.h"

#include <winsock2.h>

#include <windows.h>

#include <bcrypt.h>

namespace xbox_live_on_bds::ice::stun {

namespace {

std::uint16_t read16(std::string_view s, std::size_t at) {
    return static_cast<std::uint16_t>((std::uint8_t(s[at]) << 8) | std::uint8_t(s[at + 1]));
}
std::uint32_t read32(std::string_view s, std::size_t at) {
    return (std::uint32_t(read16(s, at)) << 16) | read16(s, at + 2);
}
void put16(std::string& out, std::uint16_t v) {
    out.push_back(char(v >> 8));
    out.push_back(char(v & 0xFF));
}
void put32(std::string& out, std::uint32_t v) {
    put16(out, std::uint16_t(v >> 16));
    put16(out, std::uint16_t(v & 0xFFFF));
}
void set16(std::string& out, std::size_t at, std::uint16_t v) {
    out[at]     = char(v >> 8);
    out[at + 1] = char(v & 0xFF);
}

} // namespace

std::optional<Header> parseHeader(std::string_view packet) {
    // First two bits zero, length a multiple of 4 that fits, the magic cookie.
    if (packet.size() < 20 || (std::uint8_t(packet[0]) & 0xC0) != 0) return std::nullopt;
    Header h{};
    h.type   = read16(packet, 0);
    h.length = read16(packet, 2);
    if (h.length % 4 != 0 || std::size_t(h.length) + 20 > packet.size() || read32(packet, 4) != MagicCookie) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < 12; ++i) h.transaction[i] = std::uint8_t(packet[8 + i]);
    return h;
}

std::optional<std::string_view> findAttribute(std::string_view packet, std::uint16_t type) {
    auto header = parseHeader(packet);
    if (!header) return std::nullopt;
    std::size_t at = 20, end = 20 + header->length;
    while (at + 4 <= end) {
        std::uint16_t t = read16(packet, at), len = read16(packet, at + 2);
        if (at + 4 + len > end) break;
        if (t == type) return packet.substr(at + 4, len);
        at += 4 + ((len + 3) & ~3u);
    }
    return std::nullopt;
}

std::string bindingSuccess(Header const& request, std::uint32_t ipv4, std::uint16_t port, std::string_view password) {
    std::string m;
    put16(m, BindingSuccess);
    put16(m, 0); // length, set below
    put32(m, MagicCookie);
    m.append(reinterpret_cast<char const*>(request.transaction.data()), request.transaction.size());

    // XOR-MAPPED-ADDRESS: family IPv4, port and address XORed with the cookie.
    put16(m, XorMappedAddress);
    put16(m, 8);
    m.push_back(0);
    m.push_back(0x01);
    put16(m, std::uint16_t(port ^ (MagicCookie >> 16)));
    std::uint32_t host = ntohl(ipv4);
    put32(m, host ^ MagicCookie);

    // MESSAGE-INTEGRITY over the message so far, with the length counting the integrity attribute.
    set16(m, 2, std::uint16_t(m.size() - 20 + 24));
    auto mac = hmacSha1(password, m);
    put16(m, MessageIntegrity);
    put16(m, 20);
    m.append(reinterpret_cast<char const*>(mac.data()), mac.size());

    // FINGERPRINT over everything before it, the length now counting the fingerprint too.
    set16(m, 2, std::uint16_t(m.size() - 20 + 8));
    std::uint32_t fp = crc32(m) ^ 0x5354554E;
    put16(m, Fingerprint);
    put16(m, 4);
    put32(m, fp);
    return m;
}

std::array<std::uint8_t, 20> hmacSha1(std::string_view key, std::string_view data) {
    std::array<std::uint8_t, 20> out{};
    BCryptHash(
        BCRYPT_HMAC_SHA1_ALG_HANDLE,
        reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
        static_cast<ULONG>(key.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
        static_cast<ULONG>(data.size()),
        out.data(),
        static_cast<ULONG>(out.size())
    );
    return out;
}

std::uint32_t crc32(std::string_view data) {
    static std::array<std::uint32_t, 256> const table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace xbox_live_on_bds::ice::stun
