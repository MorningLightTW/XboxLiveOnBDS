#pragma once

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"

#include "xbox_live_on_bds/xbl/LobbySession.h"

namespace xbox_live_on_bds {

namespace auth {
class XboxAuth;
}
class LinkMonitor;

class Mod {
public:
    static Mod& getInstance();

    Mod();
    ~Mod();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    void loadConfig();
    void startSession();
    void stopSession();

    ll::mod::NativeMod&                 mSelf;
    std::unique_ptr<auth::XboxAuth>     mAuth;
    std::unique_ptr<LinkMonitor>        mLinks;   // Players' connection quality, for the log.
    std::unique_ptr<xbl::LobbySession>  mSession; // Network work runs on its own thread.
    std::set<std::string>               mOnline;  // Players in game, by UUID (server thread only).
    std::vector<ll::event::ListenerPtr> mListeners;
    bool                                mDebug      = false;
    xbl::Visibility                     mVisibility = xbl::Visibility::FriendsOfFriends; // From config.json.
};

} // namespace xbox_live_on_bds
