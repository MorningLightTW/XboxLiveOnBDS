#include "xbox_live_on_bds/net/Http.h"

#include <fmt/format.h>

#include <windows.h>
#include <winhttp.h>

#include "xbox_live_on_bds/util/Encoding.h"

namespace xbox_live_on_bds::net {

namespace {

struct InternetHandle {
    HINTERNET h{};
    explicit InternetHandle(HINTERNET handle) : h(handle) {}
    InternetHandle(InternetHandle const&)            = delete;
    InternetHandle& operator=(InternetHandle const&) = delete;
    ~InternetHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

// One WinHTTP session for the whole process; it pools connections per host.
HINTERNET session() {
    static HINTERNET handle = [] {
        HINTERNET s = WinHttpOpen(
            L"XboxLiveOnBDS",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (s) {
            DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(s, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
            WinHttpSetTimeouts(s, 10'000, 10'000, 30'000, 30'000);
        }
        return s;
    }();
    return handle;
}

std::string failure(std::string_view what) { return fmt::format("{} failed (error {})", what, GetLastError()); }

} // namespace

std::string HttpResponse::describe() const {
    if (!error.empty()) return error;
    constexpr size_t Limit = 500;
    return fmt::format("HTTP {}: {}{}", status, body.substr(0, Limit), body.size() > Limit ? "..." : "");
}

std::string HttpResponse::describeStatus() const {
    if (!error.empty()) return error;
    return fmt::format("HTTP {}, {} bytes", status, body.size());
}

std::string HttpResponse::plainCause() const {
    if (!error.empty()) return "network error";
    if (status == 429) return "too many requests";
    if (status >= 500) return "the service is down";
    if (status == 401 || status == 403) return "access refused";
    if (status >= 400) return "request refused";
    return "unexpected answer";
}

HttpResponse request(std::string_view method, std::string_view url, Headers const& headers, std::string_view body) {
    HttpResponse res;
    HINTERNET    s = session();
    if (!s) {
        res.error = failure("WinHttpOpen");
        return res;
    }

    std::wstring   wideUrl = util::widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize      = sizeof(parts);
    parts.dwHostNameLength  = static_cast<DWORD>(-1);
    parts.dwUrlPathLength   = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
        res.error = failure("WinHttpCrackUrl");
        return res;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    InternetHandle connection{WinHttpConnect(s, host.c_str(), parts.nPort, 0)};
    if (!connection) {
        res.error = failure("WinHttpConnect");
        return res;
    }
    InternetHandle req{WinHttpOpenRequest(
        connection.h,
        util::widen(method).c_str(),
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0
    )};
    if (!req) {
        res.error = failure("WinHttpOpenRequest");
        return res;
    }

    std::wstring headerBlock;
    for (auto const& [name, value] : headers) {
        headerBlock += util::widen(name);
        headerBlock += L": ";
        headerBlock += util::widen(value);
        headerBlock += L"\r\n";
    }
    if (!WinHttpSendRequest(
            req.h,
            headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
            static_cast<DWORD>(headerBlock.size()),
            const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()),
            0
        )
        || !WinHttpReceiveResponse(req.h, nullptr)) {
        res.error = failure(fmt::format("{} {}", method, std::string(host.begin(), host.end())));
        return res;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(
        req.h,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &size,
        WINHTTP_NO_HEADER_INDEX
    );
    res.status = static_cast<int>(status);

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.h, &available)) {
            res.error = failure("WinHttpQueryDataAvailable");
            break;
        }
        if (available == 0) break;
        size_t offset = res.body.size();
        res.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, res.body.data() + offset, available, &read)) {
            res.error = failure("WinHttpReadData");
            break;
        }
        res.body.resize(offset + read);
    }
    return res;
}

} // namespace xbox_live_on_bds::net
