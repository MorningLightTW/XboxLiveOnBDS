# CLAUDE.md

Written for Claude Opus 5.5. This file holds only what that model cannot get from the code or from general knowledge. When a different model works here, check each entry against that model. Remove what it does not need, then update this line.

Written with the writing rules of ASD-STE100: short sentences, one instruction per sentence, active voice, one meaning per word. There are two exceptions. You can use words that are not in the STE dictionary. Technical names, code and commands stay as they are. Keep this style when you edit this file.

This is a LeviLamina mod for BDS on Windows only. It shows the server in the Friends tab of the host Xbox account. Friends join **BDS itself** through the NetherNet transport of BDS. They need no public IP and no port forwarding, and the mod does not transfer them to a different server. The user installs the mod and signs in one time with a device code. The mod must fix or warn about all other problems.

LeviLamina docs: [API](https://docs.levimc.org/LeviLamina/api), [C++ style guide](https://docs.levimc.org/LeviLamina/maintainer_guides/cpp_style_guide/) (naming, comments).

## Commands

```
xmake repo -u && xmake f -a x64 -m release -p windows -y   # once
git config core.hooksPath .githooks                         # once: the pre-push privacy check
xmake -y
xmake build <stun-test|udp-ports-test|signal-text-test|device-auth-test> && xmake run <same>   # device-auth-test is online
.\scripts\deploy.ps1 [-NoBuild] [-Server <folder>]         # BDS must be stopped
```

- The build works only on Windows (clang-cl, LeviLamina). On Linux, only edit and format.
- Without `-Server`, `deploy.ps1` deploys to the default server of the user. This is `$env:XBOX_LIVE_BDS_DIR`, or else the git-ignored `scripts\server-dir.txt`. Which server that is, and whether it is live, is different on each machine. Before you deploy, read `CLAUDE.local.md` or ask.
- Format with clang-format **23.1.2**. CI uses this version, and other versions give different output. A PostToolUse hook formats C++ after each edit.
- Release only with `gh workflow run release.yml -f version=X.Y.Z [-f dry_run=true]`. Do not change versions or make tags by hand. `[Unreleased]` in CHANGELOG must not be empty.

## Rules with reasons

- **Do not `leave()` the MPSD session when an RTA or signaling connection drops.** If you do, the server disappears from the Friends tab. Replace only the connection that dropped.
- Keep the session `MemberCount` at 1 or more. If it is 0, the Friends tab does not show the world.
- Each Xbox account has one activity handle. A server and the same account in a game take the Friends-tab entry from each other. Two servers on one account do the same.
- Delete the stored refresh token only when Microsoft refuses it. Keep it after a network error, a 5xx response or a 429 response.
- Xbox joins get only the TURN relays of the signaling service as ICE servers. The game does the same when it hosts a world. Other joins keep the ICE servers of BDS.
- Do not block the network thread or the main thread of BDS (hooks, event handlers). Wait only on threads of the mod.
- Do not trust remote input. nlohmann `value()` and `get<>()` throw an exception on a wrong type. Thus, put the handling of each signaling or RTA message in try/catch. Give each table with keys from remote data a size limit and an expiry time.
- `/xbl ping` and info, warn and error logs must not show IPs, ids, local paths or HTTP bodies. Users paste these logs into public issues. Use `describeStatus()` for a reply and `util::describeException` for an exception. Put the details in a debug line through `util::Redactor`.

## BDS / LeviLamina facts the code relies on

- The hooks see the transport of BDS only after it handles a signal. Thus, `bds::transport_bridge::prime` POSTs `v=0` to `/v1/join/<id>` before the mod publishes the session. BDS rejects this with CONNECTERROR and makes no peer connection. Without this step, the first join after a restart falls back to HTTP signaling. Behind NAT, that join does not complete. The bridge also uses this probe to check its guessed layouts of the signal types and `NetworkID` against `NetworkID::toString()`. On a mismatch, the mod does not publish the session.
- BDS sends its answer only after its ICE checks reach the joiner. Behind NAT, this cannot occur. Thus, the stand-in (`ice/StandIn`) answers the checks of BDS with the `ice-pwd` of the joiner. The answer of BDS contains its host and srflx candidates, IPv4 and IPv6.
- `NetherNet::ErrorOr` in the headers is smaller than the real type. Do not construct or copy it. Return the result of origin.
- If you wrap the `onComplete` of `createAnswer` in a new `brstd::move_only_function`, BDS crashes (CFG).
- If another program uses the `server-udp-ports`, BDS shows no error. It gives no candidates, and joins do not complete. Two servers on one machine need different `server-port`, `server-portv6` and `server-udp-ports`.
- `setSink()` waits for sink calls that are in progress. The sink holds a raw `Signaling::Impl*`.
- When you add a field to `Config`, increase `Config::version`. Else `ll::config::loadConfig` does not write the new field into existing `config.json` files.
- In the LeviLamina headers, you can hook or call only `MCAPI` functions. `MCNAPI` functions have no address. In the NetherNet transport, this leaves only `ReceiveFromSignalingChannel`, `SendToSignalingChannel` and `ServerNegotiator::$createAnswer`. Its constructor, `createTransport` and the virtual thunks are `MCNAPI`.
- BDS does not write `logs\latest.log` while it runs. Redirect the console to a file. PowerShell 5.1 adds a BOM to the start of piped stdin. Thus, send an empty line first.
- `"debug": true` in `config.json` or `data/debug.flag` turns on the debug log, the signaling trace and join dumps in `data/debug/`. `util::Redactor` replaces IPs, ids, names and tokens in all of them with aliases. Send each new debug line or dump through it. `data/debug-raw.flag` turns off the redactor. Its output contains IPs and identity tokens. Delete it after use.

## Log messages

These rules apply to info, warn and error lines. They follow the [Windows error message guidelines](https://learn.microsoft.com/en-us/windows/win32/debug/error-message-guidelines).

- Start with the topic and a colon, so that the owner can find the line: `Sign-in:`, `Friends tab:`, `Friends' joins:`, `Xbox Live:`, `config.json:`, `LeviLamina version:`, `Translations:`. Use one of these. Add a topic only when none fits.
- Tell what occurred, what the mod does next, and what the owner must do, if anything. Example: `config.json: could not read it (...); using the defaults`.
- Use error only when a feature stops and the mod cannot recover alone. Use warn when the mod tries again, uses a default or waits for the owner. An expected event is not an error. Examples: a sign-in code expires, or a retry occurs.
- Write a different message for each known cause. Give the cause in plain words, not only an HTTP status.
- Log a cause one time. A failure that comes from a failure already logged goes to debug, or the mod does not log it.
- The text must agree with the code. Do not tell the owner to restart when the mod tries again by itself.
- Use words the owner knows: Microsoft account, Xbox Live, Friends tab, sign-in. Put internal names (SISU, XSTS, RTA, PlayFab) and status codes in a debug line.
- Do not blame the owner. Do not use "please", "bad", capital letters for emphasis or "!".

## Conventions

- These items are out of scope. Do not add TODOs for them: automatic friend add or accept, MPSD session rotation, more than one host account.
- Use MCXboxBroadcast only as a reference for the wire format. It transfers players to a different server, which is the opposite of this design.
- In committed content, use placeholders (`Steve`, `Alex`, `test`). Do not use real gamertags, XUIDs, IPs, emails or local paths.
- Read all content before it leaves this machine. This includes a push, a release, issue or PR text, and a workflow that prints or uploads files. For a push, read `git log -p origin/main..HEAD`, with the commit messages. Look for the items in the previous rule, and for tokens, sign-in codes and `data/` contents. If you find one, stop, tell the user, and do not push.
- `.githooks/pre-push` refuses the patterns it knows (paths, emails, tokens, XUIDs, public IPs). It does not find gamertags. Do not skip it with `--no-verify` without permission from the user.
- Content that gets to GitHub stays readable after you delete it. Run the `privacy-audit` agent for a full audit. Do this before a release, before the repository becomes public, or when the user asks.
- `scripts/` contains developer tools. Change them freely. They need no backward compatibility and no deprecation path.
- If the code and a README do not agree, the code is correct. A feature change updates all three READMEs and `## [Unreleased]` in CHANGELOG.
- A CHANGELOG entry tells what changed for users. Do not tell how we did it: no commands, mechanisms or reasons.
- Write commits as Conventional Commits in English. Put one type of change in each commit and the reason in the body. Each commit must build. Put the docs that a code change needs (READMEs, CHANGELOG) in a separate `docs` commit immediately after it.
