# XboxLiveOnBDS

**English** | [繁體中文](README.zh-TW.md) | [简体中文](README.zh-CN.md)

> [!WARNING]
> **Use at your own risk.** This is not an official Microsoft or Mojang feature.
>
> - The mod signs in to Xbox Live as if it were the game itself. The host account could be restricted or banned. **Use a separate account, never your main one.**
> - `mods/xbox-live-on-bds/data/auth.json` holds the host account's sign-in and is not encrypted. Anyone who gets this file can use the account. Do not share it, and remove it before sharing or backing up the server folder.

Your friends see your Bedrock Dedicated Server (BDS) in the in-game Friends tab and join with one tap.

- **No public IP and no router setup needed.**
- The server hosts as one Xbox account (the host account). All friends of that account can join.

## Requirements

- BDS 26.60.6 with LeviLamina 26.60.8, on Windows
- An Xbox account to host as. A separate account made for this is recommended

## Install

### With lip

With the server stopped, run this in the server folder:

```
lip install github.com/MorningLightTW/XboxLiveOnBDS
```

lip puts the mod into `mods/` and installs LeviLamina too if it is missing. Update later with `lip update`. This needs lip 0.40.0 or newer (`lip --version`); an older lip answers that the package's `lip.toml` is not valid.

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

## Let players join

Once a player and the host account have added each other as friends, the server appears in the player's Friends tab.

You add friends yourself with the host account; the mod does not do it for you.

## Check your connection

`/xbl ping` shows your connection (P2P or relay, ping, packet loss) without IP addresses; operators can add `<player>` to check others.

## Settings

The defaults work as they are. The file is `mods/xbox-live-on-bds/config/config.json`:

| Field | Default | Meaning |
|---|---|---|
| `visibility` | `"friendsOfFriends"` | Who can see the server. `"friendsOfFriends"`: friends, and friends of players on the server. `"friends"`: friends only |

To limit who can join, use the server's allow list (`allow-list`).

## Common problems

**Players cannot see the server**: the console must show `Xbox Live session is up`, and the player must be friends with the host account. If it shows `does not match the NetherNet internals`, install the release made for your LeviLamina version.

**The server left the Friends tab**: the host account shows one world at a time. The server comes back about a minute after the host account leaves a world on another device.

**Players see the server but cannot join**: look for `Friends' joins:` warnings at startup (usually the allow list), and check that the game versions match.

**Players hang at "Connecting to multiplayer game"**: two servers on one computer need different `server-port`, `server-portv6` and `server-udp-ports`.

**Changing the host account**: stop the server, delete `mods/xbox-live-on-bds/data/auth.json`, then start it and sign in again.

**Reporting a problem**: create an empty `mods/xbox-live-on-bds/data/debug.flag`, restart, make the problem happen, then open a [bug report](https://github.com/MorningLightTW/XboxLiveOnBDS/issues/new?template=bug.yml). The form says what to attach.

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

The first run downloads a lot and takes a while. After `build ok`, the result is in `bin\xbox-live-on-bds\`; copy that folder into the server's `mods/`.

When updating an older version, keep `mods/xbox-live-on-bds/data/` so you do not have to sign in again.

## References

How Xbox Live and NetherNet work is not documented by Microsoft. These projects were the reference for the parts below:

- [df-mc/go-xsapi](https://github.com/df-mc/go-xsapi): reconnecting to Xbox Live's real-time service (RTA) without taking the session down, so the server stays in the Friends tab.
- [df-mc/go-nethernet](https://github.com/df-mc/go-nethernet): the relay (TURN) credentials the signaling service hands out and when they expire, waiting for them before answering a join, and using only those relays for a friend's join (as [gophertunnel](https://github.com/Sandertv/gophertunnel) does too).
- [MCXboxBroadcast](https://github.com/MCXboxBroadcast/Broadcaster): the format of the Xbox Live session a world is listed with, and how a join is handed to the server's HTTP signaling (status check first, IPv4 candidates only), which this mod only uses as a fallback. Its design is the opposite of this one: it lists a server and then transfers the player to it, while here the friend joins BDS itself.

## License

[MIT](LICENSE)
