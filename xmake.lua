add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

-- Minecraft Bedrock 26.60.6 (BDS) <-> LeviLamina 26.60.x
local levilamina_version = "26.60.8"

-- Keep lip.toml's "version" in sync.
local mod_version = "0.1.2"

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

add_requires("levilamina " .. levilamina_version, {configs = {target_type = get_config("target_type")}})
add_requires("levibuildscript")
add_requires("nlohmann_json v3.12.0", "fmt 11.2.0") -- tests only; the mod gets both through LeviLamina

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("xbox-live-on-bds")
    set_version(mod_version)
    add_defines("XBOX_LIVE_ON_BDS_VERSION=\"" .. mod_version .. "\"")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {modVersion = mod_version})
    -- The modpacker copies the DLL and manifest only; the translations go next to them.
    after_build(function (target)
        os.cp(path.join(os.projectdir(), "lang"), path.join(os.projectdir(), "bin", target:name()) .. "/")
    end)
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
        set_exceptions("none") -- To avoid conflicts with /EHa.
        add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        set_toolchains("clang-cl")
        -- The DLL names its PDB by file name only, not the build machine's full path (user name included).
        add_shflags("/PDBALTPATH:%_PDB%", {force = true})
    end
    add_packages("levilamina")
    add_syslinks("winhttp", "bcrypt", "ws2_32") -- HTTPS + WebSocket / request signing, STUN HMAC / UDP
    set_kind("shared")
    set_languages("c++23")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")

-- Pure-network check with no game or account: xmake build device-auth-test && xmake run device-auth-test
target("device-auth-test")
    set_default(false)
    set_kind("binary")
    set_languages("c++23")
    if is_plat("windows") then
        set_toolchains("clang-cl")
        add_cxflags("/utf-8")
        add_defines("NOMINMAX", "UNICODE")
    end
    add_packages("nlohmann_json", "fmt")
    add_syslinks("winhttp", "bcrypt")
    add_includedirs("src")
    add_files(
        "src/xbox_live_on_bds/net/Http.cpp",
        "src/xbox_live_on_bds/util/Encoding.cpp",
        "src/xbox_live_on_bds/auth/EcKey.cpp",
        "src/xbox_live_on_bds/auth/Signing.cpp",
        "tests/device_auth_test.cpp"
    )

-- server-udp-ports parsing and the port-in-use check, offline: xmake build udp-ports-test && xmake run udp-ports-test
target("udp-ports-test")
    set_default(false)
    set_kind("binary")
    set_languages("c++23")
    if is_plat("windows") then
        set_toolchains("clang-cl")
        add_cxflags("/utf-8")
        add_defines("NOMINMAX", "UNICODE")
    end
    add_packages("fmt")
    add_syslinks("ws2_32")
    add_includedirs("src")
    add_files("src/xbox_live_on_bds/net/UdpPorts.cpp", "tests/udp_ports_test.cpp")

-- STUN encoder vectors: xmake build stun-test && xmake run stun-test
target("stun-test")
    set_default(false)
    set_kind("binary")
    set_languages("c++23")
    if is_plat("windows") then
        set_toolchains("clang-cl")
        add_cxflags("/utf-8")
        add_defines("NOMINMAX", "UNICODE")
    end
    add_syslinks("bcrypt", "ws2_32")
    add_includedirs("src")
    add_files("src/xbox_live_on_bds/ice/Stun.cpp", "tests/stun_test.cpp")

-- What a remote player's signal must look like, offline: xmake build signal-text-test && xmake run signal-text-test
target("signal-text-test")
    set_default(false)
    set_kind("binary")
    set_languages("c++23")
    if is_plat("windows") then
        set_toolchains("clang-cl")
        add_cxflags("/utf-8")
        add_defines("NOMINMAX", "UNICODE")
    end
    add_includedirs("src")
    add_files("src/xbox_live_on_bds/xbl/SignalText.cpp", "tests/signal_text_test.cpp")

-- What the debug output hides (addresses, ids, tokens), offline: xmake build redact-test && xmake run redact-test
target("redact-test")
    set_default(false)
    set_kind("binary")
    set_languages("c++23")
    if is_plat("windows") then
        set_toolchains("clang-cl")
        add_cxflags("/utf-8")
        add_defines("NOMINMAX", "UNICODE")
    end
    add_includedirs("src")
    add_files("src/xbox_live_on_bds/util/Redact.cpp", "tests/redact_test.cpp")
