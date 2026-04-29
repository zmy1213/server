#pragma once

#include "cpp20_server/net/socket.h"
#include "cpp20_server/protocol/http.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpp20_server::net {

struct HttpClientOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{80};
    std::chrono::milliseconds receive_timeout{3000};
    std::chrono::milliseconds send_timeout{3000};
};

// Minimal blocking HTTP/1.1 client used for teaching.
// This first version opens one TCP connection per request and expects
// Content-Length based responses.
class HttpClient {
public:
    explicit HttpClient(HttpClientOptions options = {});

    [[nodiscard]] const HttpClientOptions& options() const noexcept;

    protocol::HttpResponse request(const protocol::HttpRequest& request) const;
    protocol::HttpResponse get(std::string_view path) const;
    protocol::HttpResponse post(std::string_view path,
                                std::string_view body,
                                std::string_view content_type = "text/plain; charset=utf-8") const;

private:
    SocketRuntime runtime_;
    HttpClientOptions options_;
};

} // namespace cpp20_server::net
