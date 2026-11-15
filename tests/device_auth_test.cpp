// End-to-end check of HTTPS + P-256 + the Xbox request signature, without an account or the game:
// Xbox Live only issues a device token for a correctly signed request whose ProofKey matches the signer.
//   xmake build device-auth-test && xmake run device-auth-test

#include <cstdio>

#include "xbox_live_on_bds/auth/EcKey.h"
#include "xbox_live_on_bds/auth/Signing.h"
#include "xbox_live_on_bds/net/Http.h"
#include "xbox_live_on_bds/util/Encoding.h"

using namespace xbox_live_on_bds;

int main() {
    int  failures = 0;
    auto check    = [&](bool ok, char const* what) {
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    };

    // Offline pieces.
    check(util::base64Encode("hello") == "aGVsbG8=", "base64");
    check(util::base64Encode("\xfb\xff", true) == "-_8", "base64url");
    check(util::base64Decode("aGVsbG8=") == "hello", "base64 decode");
    check(util::parseIsoTime("1970-01-02T00:00:00Z") == 86400, "iso time value");
    check(util::parseIsoTime("2000-01-01T00:00:00.4637329Z") == 946684800, "iso time with fraction");
    check(util::parseIsoTime("garbage") == 0, "iso time rejects garbage");
    check(util::randomUuid().size() == 36, "uuid");

    auto key = auth::EcKey::generate();
    check(key.has_value(), "generate P-256 key");
    if (!key) return 1;
    auto [x, y] = key->publicXY();
    check(x.size() == 32 && y.size() == 32, "public point is 2 x 32 bytes");
    auto copy = auth::EcKey::importPrivate(key->exportPrivate());
    check(copy && copy->publicXY() == key->publicXY(), "private key round trip");
    auto sig = key->signSha256("data");
    check(sig && sig->size() == 64, "signature is r||s (64 bytes)");

    // Online: device token.
    nlohmann::json body = {
        {"Properties",
         {{"AuthMethod", "ProofOfPossession"},
          {"Id", "{" + util::randomUuid() + "}"},
          {"DeviceType", "Android"},
          {"ProofKey", auth::proofKey(*key)}}      },
        {"RelyingParty", "http://auth.xboxlive.com"},
        {"TokenType",    "JWT"                     }
    };
    std::string text = body.dump();
    auto        res  = net::request(
        "POST",
        "https://device.auth.xboxlive.com/device/authenticate",
        {
            {"Content-Type", "application/json"},
            {"x-xbl-contract-version", "1"},
            {"Signature", auth::signRequest(*key, "/device/authenticate", text)}
    },
        text
    );
    bool gotToken = res.ok() && res.body.find("\"Token\"") != std::string::npos;
    check(gotToken, "Xbox Live accepted the signed device request");
    if (!gotToken) std::printf("       %s\n", res.describe().c_str());
    else {
        auto j = nlohmann::json::parse(res.body);
        std::printf("       device token expires %s\n", j.value("NotAfter", "?").c_str());
    }

    std::printf("%s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
