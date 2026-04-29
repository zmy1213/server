#include "cpp20_server/net/http_client.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace cpp20_server::net {

namespace {

std::string lower_copy(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool header_name_equals(std::string_view lhs, std::string_view rhs) {
    return lower_copy(lhs) == lower_copy(rhs);
}

void set_socket_timeout(socket_t fd, int option, std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds{0}) {
        return;
    }

#if defined(_WIN32)
    const DWORD value = static_cast<DWORD>(timeout.count());
    if (::setsockopt(fd,
                     SOL_SOCKET,
                     option,
                     reinterpret_cast<const char*>(&value),
                     sizeof(value))
        != 0) {
        throw std::system_error(last_socket_error(), "setsockopt(timeout) failed");
    }
#else
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<int>((timeout.count() % 1000) * 1000);
    if (::setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value)) != 0) {
        throw std::system_error(last_socket_error(), "setsockopt(timeout) failed");
    }
#endif
}

int send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

int safe_io_size(std::size_t value) noexcept {
    const auto max_value = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(value, max_value));
}

std::string host_header_value(std::string_view host, std::uint16_t port) {
    const bool looks_like_ipv6 = host.find(':') != std::string_view::npos && host.find(']') == std::string_view::npos;
    std::string value;
    if (looks_like_ipv6) {
        value.push_back('[');
        value.append(host);
        value.push_back(']');
    } else {
        value.append(host);
    }

    if (port != 80) {
        value.push_back(':');
        value.append(std::to_string(port));
    }
    return value;
}

std::string build_http_request(const protocol::HttpRequest& request, const HttpClientOptions& options) {
    const std::string_view method = request.method.empty() ? std::string_view{"GET"} : std::string_view{request.method};
    const std::string_view path = request.path.empty() ? std::string_view{"/"} : std::string_view{request.path};
    const std::string_view version = request.version.empty() ? std::string_view{"HTTP/1.1"}
                                                             : std::string_view{request.version};

    std::ostringstream out;
    out << method << ' ' << path << ' ' << version << "\r\n";
    out << "Host: " << host_header_value(options.host, options.port) << "\r\n";

    for (const auto& [name, value] : request.headers) {
        if (header_name_equals(name, "host")
            || header_name_equals(name, "content-length")
            || header_name_equals(name, "connection")) {
            continue;
        }
        out << name << ": " << value << "\r\n";
    }

    out << "Content-Length: " << request.body.size() << "\r\n";
    out << "Connection: close\r\n";
    out << "\r\n";
    out << request.body;
    return out.str();
}

socket_t connect_socket(std::string_view host, std::uint16_t port, const HttpClientOptions& options) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* raw_result = nullptr;
    std::string host_text{host};
    std::string port_text = std::to_string(port);
    const int rc = ::getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &raw_result);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo failed while connecting http client");
    }

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> result{raw_result, freeaddrinfo};
    std::error_code last_error;
    for (addrinfo* current = result.get(); current != nullptr; current = current->ai_next) {
        socket_t fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd == invalid_socket) {
            last_error = last_socket_error();
            continue;
        }

        if (::connect(fd, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
            try {
                set_socket_timeout(fd, SO_RCVTIMEO, options.receive_timeout);
                set_socket_timeout(fd, SO_SNDTIMEO, options.send_timeout);
                set_no_delay(fd);
                return fd;
            } catch (...) {
                close_socket(fd);
                throw;
            }
        }

        last_error = last_socket_error();
        close_socket(fd);
    }

    throw std::system_error(last_error, "http client connect failed");
}

void send_all(socket_t fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(fd,
                              data.data() + sent,
                              safe_io_size(data.size() - sent),
                              send_flags());
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }

        const auto error = last_socket_error();
        if (is_interrupted(error)) {
            continue;
        }
        throw std::system_error(error, "http client send failed");
    }
}

protocol::HttpResponse recv_response(socket_t fd) {
    std::string input;
    std::array<char, 4096> buffer{};

    for (;;) {
        if (const auto parsed = protocol::try_parse_http_response(input)) {
            return std::move(parsed->response);
        }

        const auto n = ::recv(fd, buffer.data(), safe_io_size(buffer.size()), 0);
        if (n > 0) {
            input.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }

        if (n == 0) {
            if (const auto parsed = protocol::parse_http_response(input)) {
                return std::move(*parsed);
            }
            throw std::runtime_error("http client connection closed before a complete response arrived");
        }

        const auto error = last_socket_error();
        if (is_interrupted(error)) {
            continue;
        }
        throw std::system_error(error, "http client recv failed");
    }
}

} // namespace

HttpClient::HttpClient(HttpClientOptions options) : options_(std::move(options)) {}

const HttpClientOptions& HttpClient::options() const noexcept {
    return options_;
}

protocol::HttpResponse HttpClient::request(const protocol::HttpRequest& request) const {
    socket_t fd = connect_socket(options_.host, options_.port, options_);
    try {
        const std::string raw_request = build_http_request(request, options_);
        send_all(fd, raw_request);
        auto response = recv_response(fd);
        close_socket(fd);
        return response;
    } catch (...) {
        close_socket(fd);
        throw;
    }
}

protocol::HttpResponse HttpClient::get(std::string_view path) const {
    protocol::HttpRequest request;
    request.method = "GET";
    request.path = std::string{path};
    request.version = "HTTP/1.1";
    return this->request(request);
}

protocol::HttpResponse HttpClient::post(std::string_view path,
                                        std::string_view body,
                                        std::string_view content_type) const {
    protocol::HttpRequest request;
    request.method = "POST";
    request.path = std::string{path};
    request.version = "HTTP/1.1";
    request.body = std::string{body};
    request.headers["Content-Type"] = std::string{content_type};
    return this->request(request);
}

} // namespace cpp20_server::net
