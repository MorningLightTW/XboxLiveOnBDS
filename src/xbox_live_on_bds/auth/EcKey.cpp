#include "xbox_live_on_bds/auth/EcKey.h"

#include <utility>

#include <windows.h>

#include <bcrypt.h>

namespace xbox_live_on_bds::auth {

namespace {

BCRYPT_ALG_HANDLE algorithm() {
    static BCRYPT_ALG_HANDLE handle = [] {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) alg = nullptr;
        return alg;
    }();
    return handle;
}

std::string exportBlob(void* key, LPCWSTR type) {
    ULONG size = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(key), nullptr, type, nullptr, 0, &size, 0))) {
        return {};
    }
    std::string blob(size, '\0');
    if (!BCRYPT_SUCCESS(BCryptExportKey(
            static_cast<BCRYPT_KEY_HANDLE>(key),
            nullptr,
            type,
            reinterpret_cast<PUCHAR>(blob.data()),
            size,
            &size,
            0
        ))) {
        return {};
    }
    blob.resize(size);
    return blob;
}

} // namespace

std::optional<EcKey> EcKey::generate() {
    BCRYPT_ALG_HANDLE alg = algorithm();
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!alg || !BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg, &key, 256, 0))) return std::nullopt;
    if (!BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) {
        BCryptDestroyKey(key);
        return std::nullopt;
    }
    return EcKey{key};
}

std::optional<EcKey> EcKey::importPrivate(std::string_view blob) {
    BCRYPT_ALG_HANDLE alg = algorithm();
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!alg
        || !BCRYPT_SUCCESS(BCryptImportKeyPair(
            alg,
            nullptr,
            BCRYPT_ECCPRIVATE_BLOB,
            &key,
            reinterpret_cast<PUCHAR>(const_cast<char*>(blob.data())),
            static_cast<ULONG>(blob.size()),
            0
        ))) {
        return std::nullopt;
    }
    return EcKey{key};
}

EcKey::EcKey(EcKey&& other) noexcept : mHandle(std::exchange(other.mHandle, nullptr)) {}

EcKey& EcKey::operator=(EcKey&& other) noexcept {
    if (this != &other) {
        if (mHandle) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(mHandle));
        mHandle = std::exchange(other.mHandle, nullptr);
    }
    return *this;
}

EcKey::~EcKey() {
    if (mHandle) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(mHandle));
}

std::string EcKey::exportPrivate() const { return exportBlob(mHandle, BCRYPT_ECCPRIVATE_BLOB); }

std::pair<std::string, std::string> EcKey::publicXY() const {
    // BCRYPT_ECCKEY_BLOB header, then X and Y of cbKey bytes each.
    std::string blob = exportBlob(mHandle, BCRYPT_ECCPUBLIC_BLOB);
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB)) return {};
    auto const* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB const*>(blob.data());
    size_t      n      = header->cbKey;
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + 2 * n) return {};
    return {blob.substr(sizeof(BCRYPT_ECCKEY_BLOB), n), blob.substr(sizeof(BCRYPT_ECCKEY_BLOB) + n, n)};
}

std::optional<std::string> EcKey::signSha256(std::string_view data) const {
    UCHAR hash[32];
    if (!BCRYPT_SUCCESS(BCryptHash(
            BCRYPT_SHA256_ALG_HANDLE,
            nullptr,
            0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
            static_cast<ULONG>(data.size()),
            hash,
            sizeof(hash)
        ))) {
        return std::nullopt;
    }
    std::string signature(64, '\0');
    ULONG       size = 0;
    if (!BCRYPT_SUCCESS(BCryptSignHash(
            static_cast<BCRYPT_KEY_HANDLE>(mHandle),
            nullptr,
            hash,
            sizeof(hash),
            reinterpret_cast<PUCHAR>(signature.data()),
            static_cast<ULONG>(signature.size()),
            &size,
            0
        ))) {
        return std::nullopt;
    }
    signature.resize(size);
    return signature;
}

} // namespace xbox_live_on_bds::auth
