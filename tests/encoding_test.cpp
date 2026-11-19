// UTF-8 repair checks, offline: xmake build encoding-test && xmake run encoding-test
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "xbox_live_on_bds/util/Encoding.h"

using xbox_live_on_bds::util::toValidUtf8;

namespace {
constexpr char const* Replacement = "\xEF\xBF\xBD";

bool contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

bool isValid(std::string const& text) {
    try {
        (void)nlohmann::json(text).dump();
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace

int main() {
    int  failures = 0;
    auto check    = [&](bool ok, char const* what) {
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    };
    // Valid input comes back byte for byte.
    check(toValidUtf8("").empty(), "empty");
    check(toValidUtf8("Steve's World") == "Steve's World", "ASCII unchanged");
    check(toValidUtf8("\xE4\xB8\x96\xE7\x95\x8C") == "\xE4\xB8\x96\xE7\x95\x8C", "CJK unchanged");
    check(toValidUtf8("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80", "4-byte emoji unchanged");

    // Big5 bytes, as a legacy-code-page server.properties would give them.
    std::string big5  = "\xA5\x40\xAC\xC9";
    auto        fixed = toValidUtf8(big5);
    check(contains(fixed, Replacement), "Big5: replacement character");
    check(contains(fixed, "@"), "Big5: ASCII byte kept");
    check(isValid(fixed), "Big5: result is valid");
    check(!isValid(big5), "Big5: input alone is rejected by JSON");
    bool dumped = true;
    try {
        (void)nlohmann::json(toValidUtf8(big5)).dump();
    } catch (...) {
        dumped = false;
    }
    check(dumped, "Big5: dump does not throw");

    // Ill-formed forms.
    struct Case {
        char const* what;
        std::string input;
    };
    Case const cases[] = {
        {"overlong C0 80",         std::string("\xC0\x80",         2)},
        {"overlong E0 80 80",      std::string("\xE0\x80\x80",     3)},
        {"overlong F0 80 80 80",   std::string("\xF0\x80\x80\x80", 4)},
        {"surrogate ED A0 80",     std::string("\xED\xA0\x80",     3)},
        {"above U+10FFFF",         std::string("\xF4\x90\x80\x80", 4)},
        {"byte FF",
         std::string(
             "a\xFF"
             "b",                                                  3
         )                                          },
        {"lone continuation 80",   std::string("\x80",             1)},
        {"truncated E4 B8 at end", std::string("ab\xE4\xB8",       4)},
        {"truncated F0 9F at end", std::string("\xF0\x9F",         2)},
    };
    for (auto const& c : cases) {
        auto        out  = toValidUtf8(c.input);
        std::string name = std::string(c.what) + ": replaced and valid";
        check(contains(out, Replacement) && isValid(out), name.c_str());
    }
    check(
        toValidUtf8(
            std::string(
                "a\xFF"
                "b",
                3
            )
        )
            == std::string(
                "a\xEF\xBF\xBD"
                "b"
            ),
        "bytes around a bad byte kept"
    );
    check(
        toValidUtf8(
            std::string(
                "\xE4\xB8"
                "x",
                3
            )
        )
            == std::string(
                "\xEF\xBF\xBD"
                "x"
            ),
        "resumes at the next byte"
    );

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures;
}
