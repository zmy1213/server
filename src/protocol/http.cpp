#include "cpp20_server/protocol/http.h"

#include <charconv>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <system_error>
#include <utility>

namespace cpp20_server::protocol {

namespace {

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::string lower_copy(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool parse_content_length(std::string_view value, std::size_t& length) {
    value = trim(value);
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    std::size_t parsed = 0;
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    length = parsed;
    return true;
}

void parse_header_line(HttpRequest& request, std::string_view line) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return;
    }

    const std::string name = lower_copy(trim(line.substr(0, colon)));
    const auto value = trim(line.substr(colon + 1));
    if (!name.empty()) {
        request.headers[name] = std::string{value};
    }
}

} // namespace

std::optional<HttpParseResult> try_parse_http_request(std::string_view raw) {
    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return std::nullopt;
    }

    const auto header_block = raw.substr(0, header_end);
    const auto body_start = header_end + 4;
    const auto request_line_end = header_block.find("\r\n");
    const auto request_line = request_line_end == std::string_view::npos
                                  ? header_block
                                  : header_block.substr(0, request_line_end);

    const auto first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return std::nullopt;
    }

    HttpRequest request;
    request.method = std::string{request_line.substr(0, first_space)};
    request.path = std::string{request_line.substr(first_space + 1, second_space - first_space - 1)};
    request.version = std::string{request_line.substr(second_space + 1)};

    std::size_t cursor = request_line_end == std::string_view::npos ? header_block.size() : request_line_end + 2;
    while (cursor < header_block.size()) {
        auto next = header_block.find("\r\n", cursor);
        if (next == std::string_view::npos) {
            next = header_block.size();
        }
        parse_header_line(request, header_block.substr(cursor, next - cursor));
        cursor = next + 2;
    }

    std::size_t content_length = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        if (!parse_content_length(it->second, content_length)) {
            return std::nullopt;
        }
    }

    if (raw.size() < body_start + content_length) {
        return std::nullopt;
    }

    request.body = std::string{raw.substr(body_start, content_length)};
    return HttpParseResult{std::move(request), body_start + content_length};
}

std::optional<HttpRequest> parse_http_request(std::string_view raw) {
    const auto result = try_parse_http_request(raw);
    if (!result) {
        return std::nullopt;
    }
    return result->request;
}

std::string make_http_response(int status_code,
                               std::string_view reason,
                               std::string_view body,
                               std::string_view content_type) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << ' ' << reason << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Connection: keep-alive\r\n"
             << "\r\n"
             << body;
    return response.str();
}

std::string handle_demo_http_request(const HttpRequest& request) {
    if (request.method == "GET" && request.path == "/") {
        return make_http_response(200, "OK", "hello http\n");
    }
    if (request.method == "GET" && request.path == "/health") {
        return make_http_response(200, "OK", "ok\n");
    }
    if (request.method == "POST" && request.path == "/echo") {
        return make_http_response(200, "OK", request.body);
    }

    return make_http_response(404, "Not Found", "not found\n");
}

std::string handle_demo_http_request(std::string_view raw_request) {
    const auto request = parse_http_request(raw_request);
    if (!request) {
        return make_http_response(400, "Bad Request", "bad request\n");
    }
    return handle_demo_http_request(*request);
}

void handle_demo_http_stream(net::Buffer& input, net::Buffer& output) {
    for (;;) {
        const auto parsed = try_parse_http_request(input.readable_view());
        if (!parsed) {
            // Not enough bytes yet. Leave them in input and wait for the next recv().
            return;
        }

        output.append(handle_demo_http_request(parsed->request));
        input.retrieve(parsed->bytes_consumed);
    }
}

} // namespace cpp20_server::protocol
