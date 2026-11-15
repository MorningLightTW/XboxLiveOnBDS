#include "xbox_live_on_bds/auth/Signing.h"

#include <cstdint>

#include "xbox_live_on_bds/util/Encoding.h"

namespace xbox_live_on_bds::auth {

namespace {
void putBigEndian(std::string& out, std::uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
}
} // namespace

nlohmann::json proofKey(EcKey const& key) {
    auto [x, y] = key.publicXY();
    return {
        {"kty", "EC"},
        {"alg", "ES256"},
        {"crv", "P-256"},
        {"use", "sig"},
        {"x", util::base64Encode(x, true)},
        {"y", util::base64Encode(y, true)}
    };
}

std::string signRequest(EcKey const& key, std::string_view path, std::string_view body) {
    // Windows FILETIME: 100 ns ticks since 1601-01-01.
    auto fileTime = static_cast<std::uint64_t>((util::unixNow() + 11'644'473'600LL) * 10'000'000LL);

    std::string data;
    putBigEndian(data, 1, 4); // policy version
    data.push_back('\0');
    putBigEndian(data, fileTime, 8);
    data.push_back('\0');
    data += "POST";
    data.push_back('\0');
    data += path;
    data.push_back('\0');
    data.push_back('\0'); // Authorization header: none
    data += body;
    data.push_back('\0');

    auto signature = key.signSha256(data);
    if (!signature) return {};
    std::string header;
    putBigEndian(header, 1, 4);
    putBigEndian(header, fileTime, 8);
    header += *signature;
    return util::base64Encode(header);
}

} // namespace xbox_live_on_bds::auth
