# CLAUDE.md

Written for Claude Opus 5.5: it holds only what that model cannot derive from the code or general knowledge. When a different model works here, re-check each entry against it, drop what it no longer needs, then update this line.

LeviLamina mod (Windows, BDS only) that lists the server in the host Xbox account's Friends tab. Friends join **BDS itself** over its own NetherNet transport: no public IP, no port forwarding, no transfer to another server. Install plus one device-code sign-in must be enough; anything else the mod fixes or warns about.

LeviLamina docs: [API](https://docs.levimc.org/LeviLamina/api), [C++ style guide](https://docs.levimc.org/LeviLamina/maintainer_guides/cpp_style_guide/) (naming, comments).

## Commands

```
xmake repo -u && xmake f -a x64 -m release -p windows -y   # once
xmake -y
xmake build <stun-test|udp-ports-test|signal-text-test|device-auth-test> && xmake run <same>   # device-auth-test is online
.\scripts\deploy.ps1 [-NoBuild] [-Server <folder>]         # BDS must be stopped
```

- Builds only on Windows (clang-cl, LeviLamina). On Linux: edit and format only.
- `deploy.ps1` without `-Server` deploys to the user's default server (`$env:XBOX_LIVE_BDS_DIR`, else git-ignored `scripts\server-dir.txt`). Which server that is, and whether it is live, is machine-specific: check `CLAUDE.local.md` or ask before deploying.
- Formatting: clang-format **23.1.2** (CI pins it; other versions differ). A PostToolUse hook formats C++ after every edit.
- Release only via `gh workflow run release.yml -f version=X.Y.Z [-f dry_run=true]`; never bump versions or tag by hand. `[Unreleased]` in CHANGELOG must not be empty.

## Rules with reasons

- **Never `leave()` the MPSD session on a dropped RTA or signaling connection**: it hides the server. Replace only the dropped connection.
- Session `MemberCount` must be ≥ 1, or the Friends tab hides the world.
- One activity handle per Xbox account: a server and the account playing elsewhere (or a second server on the same account) take the Friends-tab entry from each other.
- Clear the stored refresh token only when Microsoft refuses it, never on network errors, 5xx or 429.
- Xbox joins get only the signaling service's TURN relays as ICE servers (as when the game hosts); other joins keep BDS's own.
- Never block BDS's network or main thread (hooks, event handlers). Wait on the mod's own threads.
- Remote input is untrusted: nlohmann `value()`/`get<>()` throw on wrong types, so handle every signaling/RTA message in try/catch; cap and expire every table keyed by remote data.
- `/xbl ping` and logs at info level must never show IPs or ids.

## BDS / LeviLamina facts the code relies on

- The hooks see BDS's transport only after it handles a signal. `transport_bridge::prime` POSTs `v=0` to `/v1/join/<id>` before publishing (rejected with CONNECTERROR, no peer connection); without it, the first join after a restart falls back to HTTP signaling and hangs behind NAT.
- BDS releases its answer only once ICE reaches the joiner, impossible behind NAT; the stand-in (`ice/StandIn`) answers BDS's checks with the joiner's `ice-pwd`. BDS's answers carry no candidates of its own.
- `NetherNet::ErrorOr` in the headers is smaller than the real type: never construct or copy it; return origin's result.
- Wrapping `createAnswer`'s `onComplete` in a new `brstd::move_only_function` crashes BDS (CFG).
- A taken `server-udp-ports` fails silently (no candidates, joins hang). Two servers on one machine need different `server-port`, `server-portv6` and `server-udp-ports`.
- `setSink()` waits for in-flight sink calls; the sink holds a raw `Signaling::Impl*`.
- In LeviLamina's headers only `MCAPI` functions can be hooked or called; `MCNAPI` ones have no address. Of the NetherNet transport, that leaves `ReceiveFromSignalingChannel`, `SendToSignalingChannel` and `ServerNegotiator::$createAnswer` (its constructor, `createTransport` and virtual thunks are `MCNAPI`).
- `logs\latest.log` is not written while BDS runs: redirect the console. PowerShell 5.1 prefixes piped stdin with a BOM: send an empty line first.
- `data/debug.flag`: debug log, signaling trace, full join dumps in `data/debug/` (IPs and identity tokens; delete after use).

## Conventions

- Out of scope, no TODOs: auto friend add/accept, MPSD session rotation, multiple host accounts.
- MCXboxBroadcast is a wire-format reference only; it transfers players, the opposite of this design.
- Committed content uses placeholders (`Steve`, `Alex`, `test`), never real gamertags, XUIDs, IPs, emails or local paths.
- Before anything leaves this machine (push, release, issue or PR text, a workflow that prints or uploads files), read what is going out (`git log -p origin/main..HEAD`, messages included) for those and for tokens, sign-in codes and `data/` contents. Found one: stop and say so, do not push. What reaches GitHub stays readable after it is deleted.
- `scripts/` are developer tools: change them freely, no backward compatibility or deprecation path.
- Code beats README. A feature change updates all three READMEs and `## [Unreleased]` in CHANGELOG.
- Conventional Commits in English, one kind of change per commit, the why in the body, every commit builds. The docs a code change calls for (READMEs, CHANGELOG) go in their own `docs` commit right after it.
