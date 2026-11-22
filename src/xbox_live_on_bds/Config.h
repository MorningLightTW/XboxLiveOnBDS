#pragma once

#include <string>

namespace xbox_live_on_bds {

/// mods/xbox-live-on-bds/config/config.json. Every field is optional: the defaults need no editing.
struct Config {
    /// Increase it whenever a field is added, so that loadConfig rewrites older files with the new fields
    /// (their existing values are kept).
    int version = 2;

    /// Who sees the server in the Friends tab (Minecraft's Xbox Live broadcast setting):
    ///   "friendsOfFriends": the host account's friends, and friends of anyone already in the session.
    ///   "friends":          the host account's friends only.
    /// Who may actually join is up to the server: use its allow-list for that.
    std::string visibility = "friendsOfFriends";

    /// The player count in the Friends tab:
    ///   "includeHost": the players on the server plus one for the host, as when the game hosts a world, but never
    ///                  so many that a free slot shows as full.
    ///   "playersOnly": the players on the server, or 1 while nobody is on it (0 hides the world).
    std::string playerCount = "includeHost";

    /// true turns on the debug log, the NetherNet signaling trace and the join dumps in data/debug. Addresses, ids,
    /// names and tokens in them are replaced by aliases, so the output can go into a public issue.
    bool debug = false;
};

} // namespace xbox_live_on_bds
