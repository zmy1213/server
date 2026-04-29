#pragma once

#include "cpp20_server/net/buffer.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cpp20_server::protocol {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpParseResult {
    HttpRequest request;
    std::size_t bytes_consumed{0};
};

using HttpHandler = std::function<std::string(const HttpRequest& request)>;

class HttpRouter {
public:
    void add_route(std::string method, std::string path, HttpHandler handler);
    [[nodiscard]] std::string handle(const HttpRequest& request) const;

private:
    static std::string make_key(std::string_view method, std::string_view path);

    std::unordered_map<std::string, HttpHandler> routes_;
};

// Returns std::nullopt when the byte stream does not contain a complete request yet.
std::optional<HttpRequest> parse_http_request(std::string_view raw);
std::optional<HttpParseResult> try_parse_http_request(std::string_view raw);

std::string make_http_response(int status_code,
                               std::string_view reason,
                               std::string_view body,
                               std::string_view content_type = "text/plain; charset=utf-8");

// Demo routes used by examples/http_server.cpp:
// GET  /        -> hello http
// GET  /health  -> ok
// POST /echo    -> request body
std::string handle_demo_http_request(const HttpRequest& request);
std::string handle_demo_http_request(std::string_view raw_request);
void handle_demo_http_stream(net::Buffer& input, net::Buffer& output);
[[nodiscard]] const HttpRouter& demo_http_router();

} // namespace cpp20_server::protocol
