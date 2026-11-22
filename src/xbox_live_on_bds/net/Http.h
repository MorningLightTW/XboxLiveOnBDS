#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xbox_live_on_bds::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
    int         status = 0;
    std::string body;
    std::string error; // Transport failure (DNS, TLS, timeout...); empty when a response arrived.

    [[nodiscard]] bool ok() const { return error.empty() && status >= 200 && status < 300; }
    /// Short description for logs: the transport error, or the status plus the start of the body.
    [[nodiscard]] std::string describe() const;
    /// Like describe(), but never shows body text: for responses whose body may hold credentials.
    [[nodiscard]] std::string describeStatus() const;
    /// The cause in plain words, for warn and error lines: no status codes, sizes or bodies.
    [[nodiscard]] std::string plainCause() const;
    /// A network error, a 5xx or a 429: trying again later can help.
    [[nodiscard]] bool temporary() const { return !error.empty() || status >= 500 || status == 429; }
};

/// Blocking HTTPS request through WinHTTP. Never call this on the server thread.
HttpResponse request(std::string_view method, std::string_view url, Headers const& headers, std::string_view body = {});

} // namespace xbox_live_on_bds::net
