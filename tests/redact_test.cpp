// What the debug output hides (addresses, ids, tokens), offline:
// xmake build redact-test && xmake run redact-test
#include <cstdio>
#include <regex>
#include <string>
#include <string_view>

#include "xbox_live_on_bds/util/Redact.h"

using xbox_live_on_bds::util::Redactor;

namespace {

bool has(std::string const& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

// Any IPv4 or IPv6 literal left in the text.
bool hasAddress(std::string const& s) {
    static std::regex const v4(R"((^|[^0-9.])\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}([^0-9]|$))");
    static std::regex const v6(R"([0-9a-fA-F]{0,4}:[0-9a-fA-F]{0,4}:[0-9a-fA-F:]*)");
    return std::regex_search(s, v4) || std::regex_search(s, v6);
}

constexpr std::string_view Ufrag       = "Zq9xKd2f";
constexpr std::string_view Pwd         = "7hGv0aLpQe3sRt5uWx8yBc1nMd4kJf";
constexpr std::string_view Fingerprint = "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89";
constexpr std::string_view Xuid        = "2533274800000001";
constexpr std::string_view Jwt = "eyJhbGciOiJFUzI1NiJ9.eyJzdWIiOiJ0ZXN0In0.c2lnbmF0dXJlLXBsYWNlaG9sZGVyLTEyMzQ1Njc4OTA";

std::string const Offer =
    std::string(
        "v=0\r\n"
        "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "c=IN IP4 203.0.113.9\r\n"
        "a=ice-ufrag:"
    )
    + std::string(Ufrag) + "\r\na=ice-pwd:" + std::string(Pwd) + "\r\na=fingerprint:sha-256 " + std::string(Fingerprint)
    + "\r\n"
      "a=candidate:1 1 udp 2122260223 192.168.1.20 54400 typ host generation 0\r\n"
      "a=candidate:2 1 udp 2122194687 2001:db8::5 54401 typ host\r\n"
      "a=candidate:3 1 udp 1686052607 198.51.100.7 61000 typ srflx raddr 192.168.1.20 rport 54400\r\n"
      "a=candidate:4 1 udp 41885439 203.0.113.50 3478 typ relay raddr 198.51.100.7 rport 61000\r\n"
      "a=candidate:5 1 udp 2122129151 3f2a9c1e-7b3d-4c11-9a52-0d8e6f4b7a10.local 54402 typ host\r\n"
      "a=candidate:6 1 udp 1 fe80::1ff:fe23:4567:890a 54403 typ host\r\n"
      "a=candidate:7 1 udp 1 10.0.0.4 54404 typ host\r\n"
      "a=candidate:8 1 udp 1 100.64.1.2 54405 typ host\r\n"
      "a=candidate:9 1 udp 1 fd12:3456::1 54406 typ host\r\n";

} // namespace

int main() {
    int  failures = 0;
    auto check    = [&](bool ok, char const* what) {
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    };

    Redactor r;

    // An offer.
    auto offer = r.sdp(Offer);
    std::printf("%s\n", offer.c_str());
    check(!hasAddress(offer), "an offer keeps no address");
    check(
        !has(offer, Ufrag) && !has(offer, Pwd) && !has(offer, Fingerprint),
        "an offer keeps no ufrag, pwd, fingerprint"
    );
    check(
        has(offer, "a=ice-ufrag:<8 chars>") && has(offer, "a=ice-pwd:<30 chars>"),
        "credentials show their length only"
    );
    check(has(offer, "typ srflx") && has(offer, "typ relay") && has(offer, "54400"), "keeps type and port");
    check(has(offer, "(v4,private)") && has(offer, "(v4,public)"), "classes IPv4 addresses");
    check(has(offer, "(v6,public)") && has(offer, "(v6,private)"), "classes IPv6 addresses");
    check(has(offer, "mdns1") && !has(offer, ".local"), "an mDNS name is its own kind");
    check(has(offer, "c=IN IP4 ip"), "the connection line is aliased");
    check(has(offer, "\r\n") && offer.back() == '\n', "keeps the line breaks");
    check(has(offer, "raddr ip"), "raddr is aliased");
    check(!has(offer, "3f2a9c1e"), "an mDNS name is gone");
    check(has(offer, "100.64") == false && has(offer, "ip8(v4,private)"), "CGNAT is private");

    // The same address, the same alias, across calls and kinds of text.
    auto a      = r.ip("198.51.100.7");
    auto answer = r.sdp("a=candidate:1 1 udp 1 198.51.100.7 61000 typ srflx\r\n");
    check(
        has(offer, a + "(v4,public) 61000") && has(answer, a + "(v4,public) 61000"),
        "the same address keeps its alias"
    );
    check(r.text("peer 198.51.100.7:19132 left") == "peer " + a + ":19132 left", "text uses the same aliases");
    check(r.ip("198.51.100.7") == a, "ip() uses the same aliases");

    // Signals.
    auto cand = r.signal(R"(CANDIDATEADD 18446744073709551615 candidate:1 1 udp 1 203.0.113.77 5000 typ host)");
    check(
        cand.starts_with("CANDIDATEADD peer1 candidate:_ 1 udp 1 ip") && !has(cand, "203.0.113.77"),
        "a CANDIDATEADD"
    );
    check(!has(cand, "18446744073709551615"), "its connection id is hidden");
    check(r.signal("CONNECTERROR 18446744073709551615 3") == "CONNECTERROR peer1 3", "a CONNECTERROR keeps its code");
    auto request = r.signal("CONNECTREQUEST 99 " + Offer);
    check(
        request.starts_with("CONNECTREQUEST peer2 v=0") && !has(request, Ufrag) && !hasAddress(request),
        "an offer signal"
    );
    check(r.signal("garbage 198.51.100.7 here") == "garbage " + a + " here", "a malformed signal gets the text scrub");
    check(r.signal("").empty(), "an empty signal");
    check(!has(r.signal("CONNECTREQUEST 1x 203.0.113.9"), "203.0.113.9"), "a bad connection id is scrubbed");

    // Tokens and players.
    auto tokens = r.text(
        std::string("Authorization: XBL3.0 x=1234567890;") + std::string(Jwt) + R"( body {"token":")" + std::string(Jwt)
        + R"(","xuid":")" + std::string(Xuid) + R"(","hash":"0123456789abcdef0123456789abcdef"})"
    );
    std::printf("%s\n", tokens.c_str());
    check(!has(tokens, "XBL3.0 x=1234") && has(tokens, "<token>"), "an XBL3.0 header");
    check(!has(tokens, "eyJ") && !has(tokens, Xuid) && !has(tokens, "0123456789abcdef"), "a JWT, a XUID and a hex run");
    check(has(tokens, "player1"), "a XUID gets a player alias");
    auto bare = r.text(std::string("jwt ") + std::string(Jwt) + " end");
    check(bare.starts_with("jwt <token ") && bare.ends_with(" chars> end"), "a bare JWT");
    check(
        r.player(Xuid) == "player1" && r.player("Steve") == "player2" && r.player("Steve") == "player2",
        "player aliases"
    );
    check(r.id("123") == "peer3" && r.id("123") == "peer3", "id aliases");

    // IPv6 forms and look-alikes.
    auto v6 = r.text("a [2001:db8::1]:19132 b 2001:DB8:0:0:0:0:0:2 c ::1 d fe80::1%eth0 e ::ffff:192.0.2.1");
    std::printf("%s\n", v6.c_str());
    check(!hasAddress(v6), "IPv6 literals are all replaced");
    check(
        r.text("at 12:34:56, std::string and Foo::bar, v26.60.8") == "at 12:34:56, std::string and Foo::bar, v26.60.8",
        "times, scopes and versions stay"
    );
    check(r.text("host turn.example.com:3478").starts_with("host turn.example.com:3478"), "host names stay");

    // Raw mode.
    Redactor raw(false);
    check(
        raw.sdp(Offer) == Offer && raw.id("123") == "123" && raw.player("Steve") == "Steve"
            && raw.text("198.51.100.7") == "198.51.100.7" && raw.signal("CONNECTERROR 1 2") == "CONNECTERROR 1 2"
            && raw.ip("203.0.113.9") == "203.0.113.9",
        "raw mode passes everything"
    );
    raw.setEnabled(true);
    check(raw.text("198.51.100.7") == "ip1", "the switch turns redaction on");

    // A full table.
    Redactor small(true, 2);
    check(
        small.id("1") == "peer1" && small.id("2") == "peer2" && small.id("3") == "peer3" && small.id("1") == "peer4",
        "a full table is cleared and the aliases stay unique"
    );
    auto full = small.text("192.0.2.1 192.0.2.2 192.0.2.3 192.0.2.1");
    check(full == "ip1 ip2 ip3 ip4", "a full address table never answers with a question mark");

    // Odd input never throws and leaves nothing behind.
    std::string odd(5000, '\xff');
    odd += "198.51.100.7";
    check(!has(r.text(odd), "198.51.100.7"), "binary input");
    check(!has(r.sdp("a=candidate:1 1 udp x 198.51.100.7"), "198.51.100.7"), "a short candidate");
    check(!hasAddress(r.sdp("a=candidate:1 1 udp 1 198.51.100.7 1 typ host raddr")), "a candidate cut short");

    std::printf(failures == 0 ? "all passed\n" : "%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
