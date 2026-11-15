# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-11-15

### Added

- Show the server in the host Xbox account's Friends tab, so friends can join with one tap. Its player count counts the host account as one player, like a world the game hosts, but only while that stays below `max-players`, so a server with a free slot never shows as full.
- Friends join BDS directly through its own NetherNet transport: no public IP or port forwarding needed. Like a world hosted by the game, Xbox Live joins use only Microsoft's relay servers; other NetherNet joins (LAN, by address) keep the server's own STUN servers.
- Sign in once with a Microsoft device code; the sign-in is kept in `data/auth.json` and refreshed on its own, and is only dropped when Microsoft itself refuses it.
- The server stays in the Friends tab when Xbox Live closes one of its connections: the dropped connection is replaced on its own.
- `config/config.json` with a `visibility` setting (`friendsOfFriends` or `friends`).
- Startup warnings for server settings that block joins, including a `server-udp-ports` already taken by another program (for example a second server on the same computer).
- `/xbl ping`: any player can see their own connection (Xbox Live friend join or not, direct P2P or Microsoft relay, ping, jitter, packet loss), with no IP addresses, in their game language. Operators can view others with `/xbl ping <player>`.
- An empty `data/debug.flag` turns on the debug log (why a connection dropped, reconnects, per-player connection quality), a NetherNet signaling trace and join dumps.
- Each release comes with a `.sha256` file for the mod zip, to check a download against.

[Unreleased]: https://github.com/MorningLightTW/XboxLiveOnBDS/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/MorningLightTW/XboxLiveOnBDS/releases/tag/v0.1.0
