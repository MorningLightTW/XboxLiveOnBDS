#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace ll::io {
class Logger;
}

namespace xbox_live_on_bds::auth {

/// Relying party for Xbox Live services: MPSD (sessiondirectory), RTA, social, peoplehub, presence.
inline constexpr std::string_view XboxLiveRelyingParty = "http://xboxlive.com";
/// Relying party for PlayFab's LoginWithXbox (then Minecraft services and the signaling service).
inline constexpr std::string_view PlayFabRelyingParty = "https://b980a380.minecraft.playfabapi.com/";

struct XstsToken {
    std::string  token;
    std::string  userHash;
    std::string  xuid;         // Only in some relying parties' claims (http://xboxlive.com has it).
    std::string  gamertag;     // Same.
    std::int64_t notAfter = 0; // Unix seconds.

    /// Value for the Authorization header of Xbox Live requests.
    [[nodiscard]] std::string authorizationHeader() const { return "XBL3.0 x=" + userHash + ";" + token; }
};

/// Minecraft services session (authorization.franchise.minecraft-services.net), from PlayFab.
struct MinecraftSession {
    std::string  authorizationHeader; // "MCToken ..." for Minecraft services, including the signaling service.
    std::string  playerMessagingId;   // "pmid" claim: the session's SupportedConnections[].PmsgId.
    std::int64_t notAfter = 0;
};

/// Signs the host account in the way Minecraft for Android does: Microsoft account device code login,
/// then an Xbox Live device token, SISU (user + title tokens) and XSTS tokens per relying party.
/// Every token and the device key are cached in a JSON file, so a restart needs no new login.
///
/// All methods block on the network and may wait for someone to finish the device code login:
/// call them from a worker thread only. They are safe to call from several threads.
class XboxAuth {
public:
    XboxAuth(ll::io::Logger& logger, std::filesystem::path cacheFile);
    ~XboxAuth();

    /// A valid XSTS token for the relying party, signing in or refreshing first if needed.
    /// nullopt if signing in failed or `stop` was requested while waiting for the login.
    std::optional<XstsToken> getXsts(std::string_view relyingParty, std::stop_token stop);

    /// A valid Minecraft services session (PlayFab LoginWithXbox, then session/start), refreshed as needed.
    /// `gameVersion` is reported as the client's version, e.g. "1.26.60".
    std::optional<MinecraftSession> getMinecraftSession(std::string_view gameVersion, std::stop_token stop);

    /// Forgets the account (and deletes the cache), so the next getXsts starts a new device code login.
    void signOut();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace xbox_live_on_bds::auth
