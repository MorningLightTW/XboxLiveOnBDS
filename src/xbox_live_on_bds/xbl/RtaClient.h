#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "xbox_live_on_bds/net/WebSocket.h"

namespace xbox_live_on_bds::xbl {

/// Xbox Live Real-Time Activity: wss://rta.xboxlive.com/connect.
/// Subscribing to the session directory's connections gives the ConnectionId a session member needs
/// (MPSD drops the member, and with it the session, when this socket goes away), and MPSD then pushes
/// session change events here.
class RtaClient {
public:
    struct Callbacks {
        std::function<void(std::string const& connectionId)> onConnectionId;
        std::function<void(nlohmann::json const& data)>      onEvent;
        std::function<void(std::string const& reason)>       onClosed; // why the socket ended
    };

    /// Connects and subscribes to the session directory. nullptr on failure, with the reason in `error`.
    /// Callbacks run on the client's own receive thread.
    static std::unique_ptr<RtaClient>
    connect(std::string const& authorization, Callbacks callbacks, std::string& error);

    ~RtaClient();

    void               subscribe(std::string_view resourceUri);
    [[nodiscard]] bool isOpen() const { return mSocket->isOpen(); }

private:
    RtaClient(std::unique_ptr<net::WebSocket> socket, Callbacks callbacks);
    void receiveLoop();

    std::unique_ptr<net::WebSocket> mSocket;
    Callbacks                       mCallbacks;
    int                             mNextSequence = 1;
    std::mutex                      mSequenceMutex;
    std::thread                     mThread;
};

} // namespace xbox_live_on_bds::xbl
