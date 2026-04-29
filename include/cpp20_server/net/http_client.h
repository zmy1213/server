#pragma once

#include "cpp20_server/net/socket.h"
#include "cpp20_server/protocol/http.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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
    using ConnectCallback = std::function<void(std::error_code error, socket_t fd)>;

    explicit HttpClient(HttpClientOptions options = {});

    [[nodiscard]] const HttpClientOptions& options() const noexcept;

    // First async-connect version:
    // - callback runs on a background thread
    // - caller owns fd on success and must close it
    // - fd is invalid_socket on failure
    void connect_async(ConnectCallback callback,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) const;
    protocol::HttpResponse request(const protocol::HttpRequest& request) const;
    protocol::HttpResponse get(std::string_view path) const;
    protocol::HttpResponse post(std::string_view path,
                                std::string_view body,
                                std::string_view content_type = "text/plain; charset=utf-8") const;

private:
    std::shared_ptr<SocketRuntime> runtime_;
    HttpClientOptions options_;
};

} // namespace cpp20_server::net
