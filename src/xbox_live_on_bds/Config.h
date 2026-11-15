#pragma once

#include <string>

namespace xbox_live_on_bds {

/// mods/xbox-live-on-bds/config/config.json. Every field is optional: the defaults need no editing.
struct Config {
    int version = 1;

    /// Who sees the server in the Friends tab (Minecraft's Xbox Live broadcast setting):
    ///   "friendsOfFriends": the host account's friends, and friends of anyone already in the session.
    ///   "friends":          the host account's friends only.
    /// Who may actually join is up to the server: use its allow-list for that.
    std::string visibility = "friendsOfFriends";
};

} // namespace xbox_live_on_bds
