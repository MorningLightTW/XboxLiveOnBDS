#include "xbox_live_on_bds/auth/XboxAuth.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#include "ll/api/io/Logger.h"

#include "xbox_live_on_bds/auth/EcKey.h"
#include "xbox_live_on_bds/auth/Signing.h"
#include "xbox_live_on_bds/net/Http.h"
#include "xbox_live_on_bds/util/Encoding.h"
#include "xbox_live_on_bds/util/Errors.h"
#include "xbox_live_on_bds/util/Redact.h"

namespace xbox_live_on_bds::auth {

using nlohmann::json;

namespace {

// Minecraft for Android's Microsoft account client id; Xbox Live only issues SISU title tokens to title clients.
constexpr std::string_view MsaClientId         = "0000000048183522";
constexpr std::string_view MsaScope            = "service::user.auth.xboxlive.com::MBI_SSL";
constexpr std::string_view DeviceCodeUrl       = "https://login.live.com/oauth20_connect.srf";
constexpr std::string_view MsaTokenUrl         = "https://login.live.com/oauth20_token.srf";
constexpr std::string_view DeviceAuthUrl       = "https://device.auth.xboxlive.com/device/authenticate";
constexpr std::string_view DeviceAuthPath      = "/device/authenticate";
constexpr std::string_view SisuUrl             = "https://sisu.xboxlive.com/authorize";
constexpr std::string_view SisuPath            = "/authorize";
constexpr std::string_view XstsUrl             = "https://xsts.auth.xboxlive.com/xsts/authorize";
constexpr std::string_view DeviceType          = "Android";
constexpr std::string_view XblAuthRelyingParty = "http://auth.xboxlive.com";
constexpr std::string_view PlayFabTitleId      = "20CA2"; // Minecraft Bedrock
constexpr std::string_view PlayFabLoginUrl     = "https://20ca2.playfabapi.com/Client/LoginWithXbox";
constexpr std::string_view SessionStartUrl =
    "https://authorization.franchise.minecraft-services.net/api/v1.0/session/start";

// Treat tokens as expired this long before they really do, so a request never races the expiry.
constexpr std::int64_t ExpiryMargin = 5 * 60;

constexpr int CacheVersion = 1;

struct Token {
    std::string  value;
    std::int64_t notAfter = 0;
    std::string  userHash; // User token only.

    [[nodiscard]] bool valid() const { return !value.empty() && notAfter - ExpiryMargin > util::unixNow(); }
};

void to_json(json& j, Token const& t) {
    j = {
        {"value",    t.value   },
        {"notAfter", t.notAfter},
        {"userHash", t.userHash}
    };
}
void from_json(json const& j, Token& t) {
    t.value    = j.value("value", "");
    t.notAfter = j.value("notAfter", std::int64_t{0});
    t.userHash = j.value("userHash", "");
}

XstsToken parseXsts(json const& j) {
    XstsToken t;
    t.token            = j.at("Token").get<std::string>();
    t.notAfter         = util::parseIsoTime(j.at("NotAfter").get<std::string>());
    auto const& claims = j.at("DisplayClaims").at("xui").at(0);
    t.userHash         = claims.at("uhs").get<std::string>();
    t.xuid             = claims.value("xid", "");
    // The classic gamertag ("gtg") is the name Minecraft shows, with the digits of a suffixed modern gamertag
    // ("Steve1234"); "mgt" is the modern one without its suffix ("Steve").
    t.gamertag = claims.value("gtg", claims.value("mgt", ""));
    return t;
}

std::optional<json> parseJson(std::string const& body) {
    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

/// For a failed or unparseable response: the status only, as a warn or error line shows it. The body text goes to a
/// debug line, because it may hold ids or tokens.
std::string describeFailure(ll::io::Logger& logger, net::HttpResponse const& res) {
    if (!res.ok()) logger.debug("Response: {}", util::Redactor::instance().text(res.describe()));
    return res.describeStatus();
}

/// Waits `duration` unless a stop is requested first; false if it was.
bool waitFor(std::stop_token const& stop, std::chrono::seconds duration) {
    std::mutex                  m;
    std::condition_variable_any cv;
    std::unique_lock            lock(m);
    // The predicate is never true, so wait_for returns false both on a timeout and on a stop: ask the token.
    cv.wait_for(lock, stop, duration, [] { return false; });
    return !stop.stop_requested();
}

} // namespace

struct XboxAuth::Impl {
    ll::io::Logger&       logger;
    std::filesystem::path cacheFile;
    std::mutex            mutex;

    std::string                                   deviceId;
    std::optional<EcKey>                          deviceKey;
    Token                                         msa; // value = access token
    std::string                                   msaRefresh;
    Token                                         device;
    Token                                         user;
    Token                                         title;
    std::map<std::string, XstsToken, std::less<>> xsts;
    MinecraftSession                              minecraft;

    Impl(ll::io::Logger& logger, std::filesystem::path file) : logger(logger), cacheFile(std::move(file)) { load(); }

    // ---- cache ----------------------------------------------------------------------------------------------

    void load() {
        std::ifstream in(cacheFile, std::ios::binary);
        if (!in) return;
        auto j = json::parse(in, nullptr, false);
        if (j.is_discarded() || j.value("version", 0) != CacheVersion) {
            logger.warn("Ignoring unreadable Xbox Live token cache data/auth.json");
            return;
        }
        try {
            deviceId = j.value("deviceId", "");
            if (auto blob = util::base64Decode(j.value("deviceKey", ""))) deviceKey = EcKey::importPrivate(*blob);
            if (!deviceKey) deviceId.clear(); // The device token is bound to the key: both or neither.
            msa        = j.value("msa", Token{});
            msaRefresh = j.value("msaRefresh", "");
            device     = j.value("device", Token{});
            user       = j.value("user", Token{});
            title      = j.value("title", Token{});
            for (auto const& [rp, t] : j.value("xsts", json::object()).items()) {
                xsts[rp] = XstsToken{
                    t.value("token", ""),
                    t.value("userHash", ""),
                    t.value("xuid", ""),
                    t.value("gamertag", ""),
                    t.value("notAfter", std::int64_t{0})
                };
            }
            auto const mc = j.value("minecraft", json::object());
            minecraft = {mc.value("authorization", ""), mc.value("pmid", ""), mc.value("notAfter", std::int64_t{0})};
        } catch (json::exception const& e) {
            logger.warn("Ignoring broken Xbox Live token cache: {}", util::describeException(e));
            *this = Impl::Empty{};
        }
    }

    struct Empty {};
    Impl& operator=(Empty) {
        deviceId.clear();
        deviceKey.reset();
        msa = device = user = title = {};
        msaRefresh.clear();
        xsts.clear();
        minecraft = {};
        return *this;
    }

    void save() const {
        json x = json::object();
        for (auto const& [rp, t] : xsts) {
            x[rp] = {
                {"token",    t.token   },
                {"userHash", t.userHash},
                {"xuid",     t.xuid    },
                {"gamertag", t.gamertag},
                {"notAfter", t.notAfter}
            };
        }
        json j = {
            {"version",    CacheVersion                                                   },
            {"deviceId",   deviceId                                                       },
            {"deviceKey",  deviceKey ? util::base64Encode(deviceKey->exportPrivate()) : ""},
            {"msa",        msa                                                            },
            {"msaRefresh", msaRefresh                                                     },
            {"device",     device                                                         },
            {"user",       user                                                           },
            {"title",      title                                                          },
            {"xsts",       x                                                              },
            {"minecraft",
             {{"authorization", minecraft.authorizationHeader},
              {"pmid", minecraft.playerMessagingId},
              {"notAfter", minecraft.notAfter}}                                           }
        };
        std::error_code ec;
        std::filesystem::create_directories(cacheFile.parent_path(), ec);
        auto tmp  = cacheFile;
        tmp      += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << j.dump(2);
            if (!out) {
                logger.error("Could not write the Xbox Live token cache data/auth.json");
                return;
            }
        }
        std::filesystem::rename(tmp, cacheFile, ec);
        if (ec) logger.error("Could not save the Xbox Live token cache: {}", ec.message());
    }

    // ---- Microsoft account ----------------------------------------------------------------------------------

    bool storeMsa(json const& j) {
        msa.value    = j.at("access_token").get<std::string>();
        msa.notAfter = util::unixNow() + j.at("expires_in").get<std::int64_t>();
        if (j.contains("refresh_token")) msaRefresh = j["refresh_token"].get<std::string>();
        save();
        return true;
    }

    enum class Refresh { Ok, Rejected, Unreachable };

    Refresh refreshMsa() {
        auto res = net::request(
            "POST",
            MsaTokenUrl,
            {
                {"Content-Type", "application/x-www-form-urlencoded"}
        },
            util::formEncode(
                {{"client_id", std::string(MsaClientId)},
                 {"scope", std::string(MsaScope)},
                 {"grant_type", "refresh_token"},
                 {"refresh_token", msaRefresh}}
            )
        );
        auto j = parseJson(res.body);
        if (res.ok() && j) return storeMsa(*j) ? Refresh::Ok : Refresh::Rejected;
        // An outage must not cost the saved sign-in: only a refusal by Microsoft clears it, and that is an OAuth
        // error object with a 4xx. A proxy's or firewall's own page (403, 407, a 200 that is not JSON) is not one.
        bool refused = res.error.empty() && res.status >= 400 && res.status < 500 && res.status != 429 && j
                    && j->is_object() && j->contains("error");
        if (!refused) {
            logger.warn(
                "Could not reach Microsoft to refresh the sign-in ({}); will try again",
                describeFailure(logger, res)
            );
            return Refresh::Unreachable;
        }
        logger.warn("Could not refresh the Microsoft account sign-in ({}), signing in again", res.describeStatus());
        logger.debug("Microsoft sign-in refresh: {}", util::Redactor::instance().text(res.describe()));
        msaRefresh.clear();
        return Refresh::Rejected;
    }

    bool deviceCodeLogin(std::stop_token const& stop) {
        auto res = net::request(
            "POST",
            DeviceCodeUrl,
            {
                {"Content-Type", "application/x-www-form-urlencoded"}
        },
            util::formEncode(
                {{"client_id", std::string(MsaClientId)},
                 {"scope", std::string(MsaScope)},
                 {"response_type", "device_code"}}
            )
        );
        auto code = parseJson(res.body);
        if (!res.ok() || !code) {
            logger.error("Could not start the Microsoft account sign-in: {}", describeFailure(logger, res));
            return false;
        }
        auto deviceCode = code->at("device_code").get<std::string>();
        auto interval   = std::chrono::seconds(code->value("interval", 5));
        auto deadline   = util::unixNow() + code->value("expires_in", 900);

        logger.warn("==============================================================");
        logger.warn("Sign in the Xbox account this server will host as:");
        logger.warn(
            "  open {} and enter the code {}",
            code->value("verification_uri", "https://www.microsoft.com/link"),
            code->at("user_code").get<std::string>()
        );
        logger.warn("==============================================================");

        while (util::unixNow() < deadline) {
            if (!waitFor(stop, interval)) return false;
            auto poll = net::request(
                "POST",
                MsaTokenUrl,
                {
                    {"Content-Type", "application/x-www-form-urlencoded"}
            },
                util::formEncode(
                    {{"client_id", std::string(MsaClientId)},
                     {"grant_type", "device_code"},
                     {"device_code", deviceCode}}
                )
            );
            auto j = parseJson(poll.body);
            if (poll.ok() && j) {
                logger.info("Microsoft account signed in");
                return storeMsa(*j);
            }
            // Only Microsoft's own answer ends the sign-in: a network blip, a 5xx or a proxy's page must not void
            // the code the owner is typing in. Keep polling until the code expires.
            std::string error = j && j->is_object() ? j->value("error", "") : "";
            if (error.empty() || !poll.error.empty() || poll.status >= 500 || poll.status == 429) {
                logger.debug(
                    "Could not ask Microsoft about the sign-in ({}); asking again",
                    util::Redactor::instance().text(poll.ok() ? poll.describeStatus() : poll.describe())
                );
                continue;
            }
            if (error == "authorization_pending") continue;
            if (error == "slow_down") {
                interval += std::chrono::seconds(5);
                continue;
            }
            logger.error("Microsoft account sign-in failed: {}", poll.describeStatus());
            logger.debug("Microsoft sign-in poll: {}", util::Redactor::instance().text(poll.describe()));
            return false;
        }
        logger.error("The sign-in code expired before it was used; restart the mod to get a new one");
        return false;
    }

    bool ensureMsa(std::stop_token const& stop) {
        if (msa.valid()) return true;
        if (!msaRefresh.empty()) {
            switch (refreshMsa()) {
            case Refresh::Ok:
                return true;
            case Refresh::Unreachable:
                return false; // Keep the saved sign-in; the caller tries again later.
            case Refresh::Rejected:
                break;
            }
        }
        return deviceCodeLogin(stop);
    }

    // ---- Xbox Live ------------------------------------------------------------------------------------------

    bool ensureDevice() {
        if (!deviceKey) {
            deviceKey = EcKey::generate();
            if (!deviceKey) {
                logger.error("Could not create the device key (CNG ECDSA P-256)");
                return false;
            }
            deviceId = util::randomUuid();
            device   = {};
        }
        if (device.valid()) return true;

        json body = {
            {"Properties",
             {{"AuthMethod", "ProofOfPossession"},
              {"Id", "{" + deviceId + "}"},
              {"DeviceType", DeviceType},
              {"ProofKey", proofKey(*deviceKey)}}},
            {"RelyingParty", XblAuthRelyingParty },
            {"TokenType",    "JWT"               }
        };
        std::string text = body.dump();
        auto        res  = net::request(
            "POST",
            DeviceAuthUrl,
            {
                {"Content-Type", "application/json"},
                {"x-xbl-contract-version", "1"},
                {"Signature", signRequest(*deviceKey, DeviceAuthPath, text)}
        },
            text
        );
        auto j = parseJson(res.body);
        if (!res.ok() || !j) {
            logger.error("Xbox Live device authentication failed: {}", describeFailure(logger, res));
            return false;
        }
        device.value    = j->at("Token").get<std::string>();
        device.notAfter = util::parseIsoTime(j->at("NotAfter").get<std::string>());
        save();
        return true;
    }

    /// SISU: user and title tokens, plus the XSTS token for `relyingParty`. HTTP status, or 0 on success.
    int sisu(std::string_view relyingParty) {
        json body = {
            {"AccessToken",       "t=" + msa.value        },
            {"AppId",             MsaClientId             },
            {"DeviceToken",       device.value            },
            {"Sandbox",           "RETAIL"                },
            {"UseModernGamertag", true                    },
            {"SiteName",          "user.auth.xboxlive.com"},
            {"RelyingParty",      relyingParty            },
            {"ProofKey",          proofKey(*deviceKey)    }
        };
        std::string text = body.dump();
        auto        res  = net::request(
            "POST",
            SisuUrl,
            {
                {"Content-Type", "application/json"},
                {"Signature", signRequest(*deviceKey, SisuPath, text)}
        },
            text
        );
        auto j = parseJson(res.body);
        if (!res.ok() || !j) {
            logger.error("Xbox Live SISU authorization failed: {}", describeFailure(logger, res));
            return res.status == 0 ? -1 : res.status;
        }
        try {
            auto const& u                   = j->at("UserToken");
            user.value                      = u.at("Token").get<std::string>();
            user.notAfter                   = util::parseIsoTime(u.at("NotAfter").get<std::string>());
            user.userHash                   = u.at("DisplayClaims").at("xui").at(0).at("uhs").get<std::string>();
            auto const& t                   = j->at("TitleToken");
            title.value                     = t.at("Token").get<std::string>();
            title.notAfter                  = util::parseIsoTime(t.at("NotAfter").get<std::string>());
            xsts[std::string(relyingParty)] = parseXsts(j->at("AuthorizationToken"));
        } catch (json::exception const& e) {
            logger.error("Unexpected SISU response ({}): {}", util::describeException(e), res.describeStatus());
            return -1;
        }
        save();
        return 0;
    }

    /// XSTS for another relying party from the cached user/title/device tokens. HTTP status, or 0 on success.
    int authorizeXsts(std::string_view relyingParty) {
        json body = {
            {"Properties",
             {{"SandboxId", "RETAIL"},
              {"DeviceToken", device.value},
              {"TitleToken", title.value},
              {"UserTokens", json::array({user.value})}}},
            {"RelyingParty", relyingParty               },
            {"TokenType",    "JWT"                      }
        };
        auto res = net::request(
            "POST",
            XstsUrl,
            {
                {"Content-Type",           "application/json"},
                {"x-xbl-contract-version", "1"               }
        },
            body.dump()
        );
        auto j = parseJson(res.body);
        if (!res.ok() || !j) {
            // 401 with XErr 2148916233 = no Xbox profile, 2148916238 = child account needing family consent.
            if (res.status != 401)
                logger.error("XSTS authorization for {} failed: {}", relyingParty, describeFailure(logger, res));
            return res.status == 0 ? -1 : res.status;
        }
        try {
            xsts[std::string(relyingParty)] = parseXsts(*j);
        } catch (json::exception const& e) {
            logger.error("Unexpected XSTS response ({}): {}", util::describeException(e), res.describeStatus());
            return -1;
        }
        save();
        return 0;
    }

    std::optional<XstsToken> getXsts(std::string_view relyingParty, std::stop_token const& stop) {
        auto cached = [&]() -> std::optional<XstsToken> {
            if (auto it = xsts.find(relyingParty);
                it != xsts.end() && it->second.notAfter - ExpiryMargin > util::unixNow()) {
                return it->second;
            }
            return std::nullopt;
        };
        if (auto t = cached()) return t;

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (stop.stop_requested()) return std::nullopt;
            if (user.valid() && title.valid() && device.valid()) {
                int status = authorizeXsts(relyingParty);
                if (status == 0) return cached();
                if (status != 401) return std::nullopt;
                user = title = {}; // Rejected: get fresh ones through SISU.
                continue;
            }
            if (!ensureMsa(stop) || !ensureDevice()) return std::nullopt;
            int status = sisu(relyingParty);
            if (status == 0) return cached();
            if (status != 401 && status != 400) return std::nullopt;
            msa = {}; // The Microsoft account token was refused: refresh it once and retry.
        }
        return std::nullopt;
    }

    // ---- Minecraft services ---------------------------------------------------------------------------------

    std::optional<MinecraftSession> getMinecraftSession(std::string_view gameVersion, std::stop_token const& stop) {
        if (!minecraft.authorizationHeader.empty() && minecraft.notAfter - ExpiryMargin > util::unixNow()) {
            return minecraft;
        }

        auto playFabXsts = getXsts(PlayFabRelyingParty, stop);
        if (!playFabXsts) return std::nullopt;
        json login = {
            {"CreateAccount",         true                                                      },
            {"InfoRequestParameters", {{"GetPlayerProfile", true}, {"GetUserAccountInfo", true}}},
            {"TitleId",               PlayFabTitleId                                            },
            {"XboxToken",             playFabXsts->authorizationHeader()                        }
        };
        auto res = net::request(
            "POST",
            PlayFabLoginUrl,
            {
                {"Content-Type", "application/json"}
        },
            login.dump()
        );
        auto j = parseJson(res.body);
        if (!res.ok() || !j) {
            logger.error("PlayFab sign-in failed: {}", describeFailure(logger, res));
            return std::nullopt;
        }
        std::string ticket = j->value("/data/SessionTicket"_json_pointer, std::string{});
        if (ticket.empty()) {
            logger.error("PlayFab sign-in returned no session ticket: {}", res.describeStatus());
            return std::nullopt;
        }

        std::string undashedDeviceId = deviceId;
        std::erase(undashedDeviceId, '-');
        json start = {
            {"device",
             {{"applicationType", "MinecraftPE"},
              {"gameVersion", gameVersion},
              {"id", undashedDeviceId},
              {"memory", 32LL * 1024 * 1024 * 1024},
              {"hardwareMemoryTier", 5},
              {"platform", "Windows10"},
              {"playFabTitleId", PlayFabTitleId},
              {"storePlatform", "uwp.store"},
              {"type", "Windows10"}}},
            {"user",
             {{"language", "en"},
              {"regionCode", "US"},
              {"languageCode", "en-US"},
              {"tokenType", "PlayFab"},
              {"token", ticket}}    }
        };
        res = net::request(
            "POST",
            SessionStartUrl,
            {
                {"Content-Type", "application/json"}
        },
            start.dump()
        );
        j = parseJson(res.body);
        if (!res.ok() || !j) {
            logger.error("Minecraft services sign-in failed: {}", describeFailure(logger, res));
            return std::nullopt;
        }
        MinecraftSession session;
        session.authorizationHeader = j->value("/result/authorizationHeader"_json_pointer, std::string{});
        session.notAfter            = util::parseIsoTime(j->value("/result/validUntil"_json_pointer, std::string{}));
        // "MCToken <JWT>": the player messaging id is a claim of the JWT payload.
        if (auto space = session.authorizationHeader.find(' '); space != std::string::npos) {
            std::string_view jwt(session.authorizationHeader);
            jwt.remove_prefix(space + 1);
            auto first = jwt.find('.'), second = jwt.find('.', first + 1);
            if (first != std::string_view::npos && second != std::string_view::npos) {
                if (auto payload = util::base64Decode(jwt.substr(first + 1, second - first - 1))) {
                    auto claims               = json::parse(*payload, nullptr, false);
                    session.playerMessagingId = claims.is_object() ? claims.value("pmid", "") : "";
                }
            }
        }
        if (session.authorizationHeader.empty() || session.notAfter == 0) {
            logger.error("Unexpected Minecraft services response: {}", res.describeStatus());
            return std::nullopt;
        }
        minecraft = session;
        save();
        return minecraft;
    }
};

XboxAuth::XboxAuth(ll::io::Logger& logger, std::filesystem::path cacheFile)
: mImpl(std::make_unique<Impl>(logger, std::move(cacheFile))) {}

XboxAuth::~XboxAuth() = default;

std::optional<XstsToken> XboxAuth::getXsts(std::string_view relyingParty, std::stop_token stop) {
    std::lock_guard lock(mImpl->mutex);
    try {
        return mImpl->getXsts(relyingParty, stop);
    } catch (std::exception const& e) {
        // Runs on a worker thread: an escaping exception would end the whole server.
        mImpl->logger.error("Unexpected Xbox Live sign-in response: {}", util::describeException(e));
        return std::nullopt;
    }
}

std::optional<MinecraftSession> XboxAuth::getMinecraftSession(std::string_view gameVersion, std::stop_token stop) {
    std::lock_guard lock(mImpl->mutex);
    try {
        return mImpl->getMinecraftSession(gameVersion, stop);
    } catch (std::exception const& e) {
        mImpl->logger.error("Unexpected Xbox Live sign-in response: {}", util::describeException(e));
        return std::nullopt;
    }
}

void XboxAuth::signOut() {
    std::lock_guard lock(mImpl->mutex);
    *mImpl = Impl::Empty{};
    std::error_code ec;
    std::filesystem::remove(mImpl->cacheFile, ec);
}

} // namespace xbox_live_on_bds::auth
