#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "xbox_live_on_bds/net/Http.h"

namespace xbox_live_on_bds::net {

/// Blocking WebSocket client over WinHTTP (text frames). One thread may receive() while others send();
/// close() from any thread unblocks a pending receive().
class WebSocket {
public:
    /// nullptr on failure, with the reason in `error`.
    static std::unique_ptr<WebSocket> connect(std::string_view url, Headers const& headers, std::string& error);

    ~WebSocket();
    WebSocket(WebSocket const&)            = delete;
    WebSocket& operator=(WebSocket const&) = delete;

    bool send(std::string_view text);
    /// The next complete text message; nullopt once the socket is closed or broken.
    std::optional<std::string> receive();
    /// `reason` is what closeReason() reports, unless the socket already ended for another one.
    void               close(std::string_view reason = "closed by this side");
    [[nodiscard]] bool isOpen() const { return !mClosed.load(); }
    /// Why the socket ended, e.g. "the peer closed it (code 1001: ...)"; empty while it is open.
    [[nodiscard]] std::string closeReason() const;

private:
    WebSocket(void* session, void* connection, void* socket);
    void setCloseReason(std::string reason);

    void*              mConnection{}; // HINTERNET
    void*              mSocket{};     // HINTERNET
    std::mutex         mSendMutex;
    std::mutex         mCloseMutex;
    std::atomic<bool>  mClosed{false};
    mutable std::mutex mReasonMutex;
    std::string        mCloseReason; // the first reason given; guarded by mReasonMutex
};

} // namespace xbox_live_on_bds::net
