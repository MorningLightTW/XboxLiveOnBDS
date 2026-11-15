#include "xbox_live_on_bds/net/WebSocket.h"

#include <fmt/format.h>

#include <windows.h>
#include <winhttp.h>

#include "xbox_live_on_bds/util/Encoding.h"

namespace xbox_live_on_bds::net {

namespace {
std::string failure(std::string_view what, DWORD code = GetLastError()) {
    return fmt::format("{} failed (error {})", what, code);
}
} // namespace

WebSocket::WebSocket(void* /*session*/, void* connection, void* socket) : mConnection(connection), mSocket(socket) {}

std::unique_ptr<WebSocket> WebSocket::connect(std::string_view url, Headers const& headers, std::string& error) {
    // wss:// is https:// as far as WinHTTP's URL parser is concerned.
    std::string httpUrl(url);
    if (httpUrl.starts_with("wss://")) httpUrl.replace(0, 6, "https://");
    else if (httpUrl.starts_with("ws://")) httpUrl.replace(0, 5, "http://");

    static HINTERNET session = [] {
        HINTERNET s = WinHttpOpen(
            L"XboxLiveOnBDS",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (s) WinHttpSetTimeouts(s, 10'000, 10'000, 30'000, 0); // receive: wait forever for messages
        return s;
    }();
    if (!session) {
        error = failure("WinHttpOpen");
        return nullptr;
    }

    std::wstring   wideUrl = util::widen(httpUrl);
    URL_COMPONENTS parts{};
    parts.dwStructSize      = sizeof(parts);
    parts.dwHostNameLength  = static_cast<DWORD>(-1);
    parts.dwUrlPathLength   = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
        error = failure("WinHttpCrackUrl");
        return nullptr;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connection) {
        error = failure("WinHttpConnect");
        return nullptr;
    }
    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0
    );
    auto fail = [&](std::string message) -> std::unique_ptr<WebSocket> {
        error = std::move(message);
        if (request) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        return nullptr;
    };
    if (!request) return fail(failure("WinHttpOpenRequest"));
    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        return fail(failure("WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET"));
    }

    std::wstring headerBlock;
    for (auto const& [name, value] : headers) {
        headerBlock += util::widen(name) + L": " + util::widen(value) + L"\r\n";
    }
    if (!WinHttpSendRequest(
            request,
            headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
            static_cast<DWORD>(headerBlock.size()),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0
        )
        || !WinHttpReceiveResponse(request, nullptr)) {
        return fail(failure(fmt::format("WebSocket handshake with {}", url)));
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &size,
        WINHTTP_NO_HEADER_INDEX
    );
    if (status != 101) return fail(fmt::format("WebSocket handshake with {} got HTTP {}", url, status));

    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    request = nullptr;
    if (!socket) return fail(failure("WinHttpWebSocketCompleteUpgrade"));

    return std::unique_ptr<WebSocket>(new WebSocket(session, connection, socket));
}

WebSocket::~WebSocket() {
    close(); // Closes mSocket.
    std::lock_guard lock(mCloseMutex);
    if (mConnection) WinHttpCloseHandle(mConnection);
}

bool WebSocket::send(std::string_view text) {
    if (mClosed) return false;
    std::lock_guard lock(mSendMutex);
    DWORD           result = WinHttpWebSocketSend(
        mSocket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(text.data()),
        static_cast<DWORD>(text.size())
    );
    if (result != NO_ERROR) {
        setCloseReason(fmt::format("sending failed (error {})", result));
        close();
        return false;
    }
    return true;
}

void WebSocket::setCloseReason(std::string reason) {
    std::lock_guard lock(mReasonMutex);
    if (mCloseReason.empty()) mCloseReason = std::move(reason);
}

std::string WebSocket::closeReason() const {
    std::lock_guard lock(mReasonMutex);
    return mCloseReason;
}

std::optional<std::string> WebSocket::receive() {
    std::string message;
    char        buffer[8192];
    for (;;) {
        if (mClosed) return std::nullopt;
        DWORD                          read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        DWORD                          result = WinHttpWebSocketReceive(mSocket, buffer, sizeof(buffer), &read, &type);
        if (result != NO_ERROR) {
            // After a close() here this is the cancelled receive, and the reason is already set.
            setCloseReason(fmt::format("the connection broke (error {})", result));
            close();
            return std::nullopt;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT status = 0;
            BYTE   text[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH]{};
            DWORD  length = 0;
            if (WinHttpWebSocketQueryCloseStatus(mSocket, &status, text, sizeof(text), &length) == NO_ERROR) {
                std::string why(reinterpret_cast<char const*>(text), length);
                setCloseReason(
                    why.empty() ? fmt::format("the peer closed it (code {})", status)
                                : fmt::format("the peer closed it (code {}: {})", status, why)
                );
            } else {
                setCloseReason("the peer closed it");
            }
            close();
            return std::nullopt;
        }
        message.append(buffer, read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
            || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            return message;
        }
        // *_FRAGMENT_BUFFER_TYPE: keep reading the rest of the message.
    }
}

void WebSocket::close(std::string_view reason) {
    setCloseReason(std::string(reason));
    if (mClosed.exchange(true)) return;
    std::lock_guard lock(mCloseMutex);
    // Shutdown sends the close frame without waiting for the peer's; then cancel any blocked receive.
    // mSocket keeps its (now dead) value: another thread may be reading it, and calls on it simply fail.
    WinHttpWebSocketShutdown(mSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    WinHttpCloseHandle(mSocket);
}

} // namespace xbox_live_on_bds::net
