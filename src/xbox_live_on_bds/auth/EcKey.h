#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace xbox_live_on_bds::auth {

/// ECDSA P-256 key pair (Windows CNG). Xbox Live binds the device token and SISU tokens to it
/// ("ProofKey") and expects every request that uses them to be signed with it.
class EcKey {
public:
    static std::optional<EcKey> generate();
    /// From exportPrivate()'s output.
    static std::optional<EcKey> importPrivate(std::string_view blob);

    EcKey(EcKey&& other) noexcept;
    EcKey& operator=(EcKey&& other) noexcept;
    EcKey(EcKey const&)            = delete;
    EcKey& operator=(EcKey const&) = delete;
    ~EcKey();

    /// BCRYPT_ECCPRIVATE_BLOB bytes, for the token cache.
    [[nodiscard]] std::string exportPrivate() const;
    /// The public point's X and Y, 32 bytes each, big endian.
    [[nodiscard]] std::pair<std::string, std::string> publicXY() const;
    /// SHA-256 then ECDSA; returns r || s (IEEE P1363, 64 bytes).
    [[nodiscard]] std::optional<std::string> signSha256(std::string_view data) const;

private:
    explicit EcKey(void* handle) : mHandle(handle) {}
    void* mHandle{}; // BCRYPT_KEY_HANDLE
};

} // namespace xbox_live_on_bds::auth
