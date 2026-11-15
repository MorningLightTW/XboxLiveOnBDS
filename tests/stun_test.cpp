// STUN encoder checks against published vectors: xmake build stun-test && xmake run stun-test
#include <cstdio>
#include <string>

#include "xbox_live_on_bds/ice/Stun.h"

using namespace xbox_live_on_bds::ice;

int main() {
    int  failures = 0;
    auto check    = [&](bool ok, char const* what) {
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    };
    check(stun::crc32("123456789") == 0xCBF43926u, "CRC-32 check value");
    auto        mac = stun::hmacSha1("Jefe", "what do ya want for nothing?"); // RFC 2202 test case 2
    std::string hex;
    for (auto b : mac) {
        char t[3];
        std::snprintf(t, sizeof t, "%02x", b);
        hex += t;
    }
    check(hex == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", "HMAC-SHA1 RFC 2202 #2");

    // A request, then the response: it must parse, carry our transaction, and its integrity/fingerprint must verify.
    std::string request(20, '\0');
    request[1] = 0x01; // Binding request, length 0
    request[4] = 0x21;
    request[5] = 0x12;
    request[6] = char(0xA4);
    request[7] = 0x42;
    for (int i = 0; i < 12; ++i) request[8 + i] = char(i + 1);
    auto header = stun::parseHeader(request);
    check(header.has_value() && header->type == stun::BindingRequest, "parse request");
    auto reply = stun::bindingSuccess(*header, 0x0100007F /* 127.0.0.1 */, 50000, "pass");
    auto rh    = stun::parseHeader(reply);
    check(rh && rh->type == stun::BindingSuccess && rh->transaction == header->transaction, "response header");
    auto fp = stun::findAttribute(reply, stun::Fingerprint);
    check(fp && fp->size() == 4, "fingerprint present");
    std::string before = reply.substr(0, reply.size() - 8);
    unsigned    got    = (unsigned char)(*fp)[0] << 24 | (unsigned char)(*fp)[1] << 16 | (unsigned char)(*fp)[2] << 8
                       | (unsigned char)(*fp)[3];
    check(got == (stun::crc32(before) ^ 0x5354554Eu), "fingerprint verifies");
    auto        mi         = stun::findAttribute(reply, stun::MessageIntegrity);
    std::string signedPart = reply.substr(0, reply.size() - 8 - 24);
    signedPart[2]          = 0;
    signedPart[3]          = char(reply.size() - 20 - 8); // length as when signed: up to and incl. MI
    auto expect            = stun::hmacSha1("pass", signedPart);
    check(mi && std::string(mi->begin(), mi->end()) == std::string(expect.begin(), expect.end()), "integrity verifies");
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
