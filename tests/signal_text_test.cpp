// What a remote player's signal must look like to be handled, offline:
// xmake build signal-text-test && xmake run signal-text-test
#include <cstdio>
#include <string_view>

#include "xbox_live_on_bds/xbl/SignalText.h"

using namespace xbox_live_on_bds::xbl;

int main() {
    int  failures = 0;
    auto check    = [&](bool ok, char const* what) {
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    };

    auto offer = parseSignal("CONNECTREQUEST 12345 v=0\r\na=ice-ufrag:abcd\r\n");
    check(
        offer && offer->type == "CONNECTREQUEST" && offer->connectionId == "12345"
            && offer->payload == "v=0\r\na=ice-ufrag:abcd\r\n",
        "splits an offer"
    );
    auto candidate = parseSignal("CANDIDATEADD 18446744073709551615 candidate:1 1 udp 1 192.0.2.1 5 typ host");
    check(
        candidate && candidate->connectionId == "18446744073709551615"
            && candidate->payload == "candidate:1 1 udp 1 192.0.2.1 5 typ host",
        "keeps the spaces of the payload, takes the largest connection id"
    );
    auto error = parseSignal("CONNECTERROR 7 ");
    check(error && error->payload.empty(), "an empty payload is still a signal");

    check(!parseSignal(""), "rejects nothing");
    check(!parseSignal("CONNECTREQUEST"), "rejects a type alone");
    check(!parseSignal("CONNECTREQUEST 12345"), "rejects a signal without a payload part");
    check(!parseSignal(" 12345 v=0"), "rejects a missing type");
    check(!parseSignal("CONNECTREQUEST  v=0"), "rejects an empty connection id");
    check(!parseSignal("CONNECTREQUEST 12a45 v=0"), "rejects a connection id with letters");
    check(!parseSignal("CONNECTREQUEST -5 v=0"), "rejects a negative connection id");
    check(!parseSignal("CONNECTREQUEST 1/../../x?y=1 v=0"), "rejects a connection id shaped like a path");
    check(!parseSignal("CONNECTREQUEST 1\r\nHost: v=0"), "rejects a connection id with a line break");
    check(!parseSignal("CONNECTREQUEST 18446744073709551616 v=0"), "rejects a connection id past 64 bits");

    check(parseNetherNetId("9876543210") == 9876543210ull, "reads a NetherNet id");
    check(!parseNetherNetId(""), "rejects an empty NetherNet id");
    check(!parseNetherNetId("0"), "rejects NetherNet id 0");
    check(!parseNetherNetId("12 "), "rejects a trailing space");
    check(!parseNetherNetId("1.5e3"), "rejects a number that is not an integer");
    check(!parseNetherNetId("12/../x"), "rejects a NetherNet id shaped like a path");
    check(!parseNetherNetId("18446744073709551616"), "rejects a NetherNet id past 64 bits");

    std::printf(failures == 0 ? "all passed\n" : "%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
