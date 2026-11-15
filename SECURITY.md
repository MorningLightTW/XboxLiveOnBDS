# Security policy

## Supported versions

Only the latest release gets fixes.

## Reporting a vulnerability

Please do not open a public issue. Report it privately through
[GitHub's private vulnerability reporting](https://github.com/MorningLightTW/XboxLiveOnBDS/security/advisories/new)
(Security tab > Report a vulnerability), with the version, what an attacker can do, and how to reproduce it.

Things worth reporting include:

- a player, or anyone on the network, being able to crash the server, take over another player's join, or reach
  the server in a way it does not allow;
- the host account's sign-in (`mods/xbox-live-on-bds/data/auth.json`, its tokens) leaking through logs, the
  Xbox Live session or any other way.

Out of scope: someone who already has `auth.json` or access to the server folder; Xbox Live or Minecraft
accounts being restricted for using an unofficial sign-in (see the warning in the README).

Please give a fix time before publishing details.
