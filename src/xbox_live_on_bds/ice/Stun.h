#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xbox_live_on_bds::ice {

/// The bits of STUN (RFC 8489) and ICE connectivity checks (RFC 8445) the mod needs.
namespace stun {

inline constexpr std::uint32_t MagicCookie = 0x2112A442;

enum MessageType : std::uint16_t {
    BindingRequest = 0x0001,
    BindingSuccess = 0x0101,
};

enum Attribute : std::uint16_t {
    Username         = 0x0006,
    MessageIntegrity = 0x0008,
    XorMappedAddress = 0x0020,
    Fingerprint      = 0x8028,
};

using TransactionId = std::array<std::uint8_t, 12>;

struct Header {
    std::uint16_t type;
    std::uint16_t length;
    TransactionId transaction;
};

/// The header of a STUN message, or nullopt if `packet` is not one (DTLS, SCTP, garbage...).
std::optional<Header> parseHeader(std::string_view packet);

/// The value of the first attribute `type` in `packet`, if any.
std::optional<std::string_view> findAttribute(std::string_view packet, std::uint16_t type);

/// A Binding success response to `request`, from the side whose ICE password is `password`, telling the
/// requester it was seen at `ipv4` (network order) : `port` (host order).
std::string bindingSuccess(Header const& request, std::uint32_t ipv4, std::uint16_t port, std::string_view password);

/// HMAC-SHA1 (STUN MESSAGE-INTEGRITY).
std::array<std::uint8_t, 20> hmacSha1(std::string_view key, std::string_view data);

/// CRC-32 as STUN's FINGERPRINT uses it (before the XOR with 0x5354554E).
std::uint32_t crc32(std::string_view data);

} // namespace stun
} // namespace xbox_live_on_bds::ice
