# XboxLiveOnBDS

**English** | [繁體中文](README.zh-TW.md) | [简体中文](README.zh-CN.md)

> [!WARNING]
> **Use at your own risk.** This is not an official Microsoft or Mojang feature.
>
> - The mod signs in to Xbox Live as if it were the game itself. The host account could be restricted or banned. **Use a separate account, never your main one.**
> - `mods/xbox-live-on-bds/data/auth.json` holds the host account's sign-in and is not encrypted. Anyone who gets this file can use the account. Do not share it, and remove it before sharing or backing up the server folder.

Adds friend-hosted worlds to Bedrock Dedicated Server (BDS): the server shows up in the Friends tab like a world a friend is hosting, and friends join with one tap.

## Requirements

- BDS 26.60.6 with LeviLamina 26.60.8, on Windows
- An Xbox account to host as. A separate account made for this is recommended

## Install

### With lip

With the server stopped, run this in the server folder:

```
lip install github.com/MorningLightTW/XboxLiveOnBDS
```

Needs lip 0.40.0 or newer. lip also installs LeviLamina if it is missing. Update with `lip update`.

### By hand

1. Stop the server.
2. Download `XboxLiveOnBDS-server-windows-x64.zip` from [Releases](https://github.com/MorningLightTW/XboxLiveOnBDS/releases).
3. Put the `xbox-live-on-bds` folder from the zip into the server's `mods/` folder.
4. Start the server.

To build the mod files yourself, see [Build it yourself](#build-it-yourself) at the bottom.

## Sign in

On the first start, the console shows a link and a code:

```
Sign in the Xbox account this server will host as:
  open https://www.microsoft.com/link and enter the code ABCD1234
```

Open the link in a browser, enter the code, and sign in with the host account. You are done when this line appears:

```
Xbox Live session is up: ...
```

Later restarts do not ask you to sign in again.

A code is good for about 15 minutes. After two unused codes, type `xbl login` in the server console for a new one.

## Let players join

Add each player as a friend with the host account (the mod does not do this). The server then shows in their Friends tab.

## Check your connection

`/xbl ping` shows your connection (P2P or relay, ping, packet loss) without IP addresses; operators can add `<player>` to check others.

## Settings

The defaults work as they are. The file is `mods/xbox-live-on-bds/config/config.json`:

| Field | Default | Meaning |
|---|---|---|
| `visibility` | `"friendsOfFriends"` | Who sees the server<br>`"friends"`: friends only<br>`"friendsOfFriends"`: also friends of players on it |
| `playerCount` | `"includeHost"` | The count in the Friends tab<br>`"includeHost"`: players + 1 for the host<br>`"playersOnly"`: players only (at least 1) |
| `debug` | `false` | `true`: detailed log for a bug report |

To limit who can join, use the server's allow list (`allow-list`).

## Common problems

**Players cannot see the server**: the console must show `Xbox Live session is up`, and the player must be a friend of the host account. On `does not match the NetherNet internals` or `lacks NetherNet functions`, install the release for your LeviLamina version (the message names it).

**The server left the Friends tab**: the host account shows one world at a time. The server comes back about a minute after the host account leaves a world on another device.

**Players see the server but cannot join**: look for `Friends' joins:` warnings at startup (usually the allow list), and check that the game versions match.

**Players hang at "Connecting to multiplayer game"**: two servers on one computer need different `server-port`, `server-portv6` and `server-udp-ports`.

**Changing the host account**: stop the server, delete `mods/xbox-live-on-bds/data/auth.json`, then start it and sign in again.

**Reporting a problem**: set `"debug": true` in `config.json`, restart, make the problem happen, then open a [bug report](https://github.com/MorningLightTW/XboxLiveOnBDS/issues/new?template=bug.yml).

## Build it yourself

You need Windows with these tools installed:

- [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) (select "Desktop development with C++")
- [LLVM](https://github.com/llvm/llvm-project/releases)
- [xmake](https://xmake.io)
- [Git](https://git-scm.com)

Open a terminal in the project folder and run:

```powershell
xmake f -y -p windows -a x64 -m release
xmake -y
```

After `build ok`, copy `bin\xbox-live-on-bds\` into the server's `mods/`. To update, keep `mods/xbox-live-on-bds/data/`: it holds the sign-in.

## References

Microsoft does not document Xbox Live or NetherNet. References:

- [df-mc/go-xsapi](https://github.com/df-mc/go-xsapi): reconnecting to RTA without taking the session down.
- [df-mc/go-nethernet](https://github.com/df-mc/go-nethernet): the relay (TURN) credentials, and using only those relays for friends' joins (as [gophertunnel](https://github.com/Sandertv/gophertunnel) does).
- [MCXboxBroadcast](https://github.com/MCXboxBroadcast/Broadcaster): the Xbox Live session format and the HTTP signaling fallback. It transfers players to another server; here friends join BDS itself.

## License

[MIT](LICENSE)
