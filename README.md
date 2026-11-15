# XboxLiveOnBDS

**English** | [繁體中文](README.zh-TW.md) | [简体中文](README.zh-CN.md)

> [!WARNING]
> **Use at your own risk.** This is not an official Microsoft or Mojang feature.
>
> - The mod signs in to Xbox Live as if it were the game itself. The host account could be restricted or banned. **Use a separate account, never your main one.**
> - `mods/xbox-live-on-bds/data/auth.json` holds the host account's sign-in and is not encrypted. Anyone who gets this file can use the account. Do not share it, and remove it before sharing or backing up the server folder.

Makes a Bedrock Dedicated Server (BDS) show up in players' in-game Friends tab, so they can join with one tap.

**No public IP and no router setup needed.**

The server hosts as an Xbox account (the host account), and that account's friends can see and join it.

## Requirements

- BDS 26.60.6 with LeviLamina 26.60.7, on Windows
- An Xbox account to host as. A separate account made for this is recommended

## Install

### With lip

With the server stopped, run this in the server folder:

```
lip install github.com/MorningLightTW/XboxLiveOnBDS
```

lip puts the mod into `mods/` and installs LeviLamina too if it is missing. Update later with `lip update`.

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

Any player can run `/xbl ping` to see their own connection: whether they came in from the Xbox Live Friends tab, whether it is direct (P2P) or through Microsoft's relay, ping, jitter and packet loss. It shows no IP addresses. Operators (and the console) can look at others with `/xbl ping <player>`. The text, and the command's description in the command list, follow the player's game language (English, Traditional Chinese, Simplified Chinese; others get English). The translations are in `mods/xbox-live-on-bds/lang/`.

## Settings

The defaults work as they are. The file is `mods/xbox-live-on-bds/config/config.json`:

| Field | Default | Meaning |
|---|---|---|
| `visibility` | `"friendsOfFriends"` | Who can see the server. `"friendsOfFriends"`: friends, and friends of players on the server. `"friends"`: friends only |

To limit who can join, use the server's allow list (`allow-list`).

## Common problems

**Players cannot see the server**
Check that the console shows `Xbox Live session is up` and that the player and the host account are friends. A player may need to restart the game after adding the friend.

**The server left the Friends tab while the host account was playing elsewhere**
The host account can show only one world in the Friends tab. While it is in another world on another device, the server stays hidden and the console warns about it. Within about a minute after that device leaves the world, the server comes back on its own. Better not to play on the host account at all.

**Players see the server but cannot join**
Look for warnings starting with `Friends' joins:` at startup; usually the allow list is on. Also check that the player's game version matches the server's.

**Players hang at "Connecting to multiplayer game"**
When two servers run on one computer, each needs its own `server-port`, `server-portv6` and `server-udp-ports` in `server.properties`. If the UDP port is already taken, the server cannot reach players and says nothing about it; this mod warns with `Friends' joins: server-udp-ports=... is already in use` at startup.

**Changing the host account**
Stop the server, delete `mods/xbox-live-on-bds/data/auth.json`, then start it and sign in again.

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
