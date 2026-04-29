#pragma once

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

// Returns std::nullopt when the byte stream does not contain a complete request yet.
std::optional<HttpRequest> parse_http_request(std::string_view raw);

std::string make_http_response(int status_code,
                               std::string_view reason,
                               std::string_view body,
                               std::string_view content_type = "text/plain; charset=utf-8");

// Demo routes used by examples/http_server.cpp:
// GET  /        -> hello http
// GET  /health  -> ok
// POST /echo    -> request body
std::string handle_demo_http_request(std::string_view raw_request);

} // namespace cpp20_server::protocol
