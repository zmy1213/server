#include "cpp20_server/net/socket.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <mstcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace cpp20_server::net {

SocketRuntime::SocketRuntime() {
#if defined(_WIN32)
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        throw std::system_error(rc, std::system_category(), "WSAStartup failed");
    }
#endif
}

SocketRuntime::~SocketRuntime() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

std::error_code last_socket_error() noexcept {
#if defined(_WIN32)
    return {WSAGetLastError(), std::system_category()};
#else
    return {errno, std::generic_category()};
#endif
}

bool is_would_block(std::error_code error) noexcept {
#if defined(_WIN32)
    return error.value() == WSAEWOULDBLOCK;
#else
    return error.value() == EAGAIN || error.value() == EWOULDBLOCK;
#endif
}

bool is_interrupted(std::error_code error) noexcept {
#if defined(_WIN32)
    return error.value() == WSAEINTR;
#else
    return error.value() == EINTR;
#endif
}

bool is_connect_in_progress(std::error_code error) noexcept {
#if defined(_WIN32)
    return error.value() == WSAEWOULDBLOCK
           || error.value() == WSAEINPROGRESS
           || error.value() == WSAEALREADY;
#else
    return error.value() == EINPROGRESS || error.value() == EALREADY;
#endif
}

std::error_code socket_pending_error(socket_t fd) noexcept {
    if (fd == invalid_socket) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }

    int value = 0;
#if defined(_WIN32)
    int length = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &length) != 0) {
        return last_socket_error();
    }
    if (value == 0) {
        return {};
    }
    return {value, std::system_category()};
#else
    socklen_t length = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &value, &length) != 0) {
        return last_socket_error();
    }
    if (value == 0) {
        return {};
    }
    return {value, std::generic_category()};
#endif
}

void close_socket(socket_t fd) noexcept {
    if (fd == invalid_socket) {
        return;
    }

#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
}

void set_non_blocking(socket_t fd) {
#if defined(_WIN32)
    u_long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        throw std::system_error(last_socket_error(), "ioctlsocket(FIONBIO) failed");
    }
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::system_error(last_socket_error(), "fcntl(F_GETFL) failed");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::system_error(last_socket_error(), "fcntl(F_SETFL) failed");
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        throw std::system_error(last_socket_error(), "fcntl(F_SETFD) failed");
    }
#endif
}

void set_reuse_addr(socket_t fd) {
    int enabled = 1;
    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   reinterpret_cast<const char*>(&enabled),
                   sizeof(enabled)) != 0) {
        throw std::system_error(last_socket_error(), "setsockopt(SO_REUSEADDR) failed");
    }
}

void set_no_delay(socket_t fd) {
    int enabled = 1;
    if (setsockopt(fd,
                   IPPROTO_TCP,
                   TCP_NODELAY,
                   reinterpret_cast<const char*>(&enabled),
                   sizeof(enabled)) != 0) {
        throw std::system_error(last_socket_error(), "setsockopt(TCP_NODELAY) failed");
    }
}

namespace {

class AddrInfo {
public:
    explicit AddrInfo(struct addrinfo* value) : value_(value) {}

    ~AddrInfo() {
        if (value_ != nullptr) {
            freeaddrinfo(value_);
        }
    }

    AddrInfo(const AddrInfo&) = delete;
    AddrInfo& operator=(const AddrInfo&) = delete;

    [[nodiscard]] struct addrinfo* get() const noexcept {
        return value_;
    }

private:
    struct addrinfo* value_{nullptr};
};

void try_enable_dual_stack(socket_t fd, int family) noexcept {
    if (family != AF_INET6) {
        return;
    }

#if defined(IPV6_V6ONLY)
    int disabled = 0;
    setsockopt(fd,
               IPPROTO_IPV6,
               IPV6_V6ONLY,
               reinterpret_cast<const char*>(&disabled),
               sizeof(disabled));
#endif
}

} // namespace

socket_t create_listening_socket(std::string_view host, std::uint16_t port, int backlog) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    std::string host_string{host};
    std::string port_string = std::to_string(port);
    struct addrinfo* raw_result = nullptr;
    const char* node = host_string.empty() ? nullptr : host_string.c_str();
    const int rc = getaddrinfo(node, port_string.c_str(), &hints, &raw_result);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo failed: " + std::to_string(rc));
    }

    AddrInfo result{raw_result};
    std::error_code last_error;

    for (struct addrinfo* current = result.get(); current != nullptr; current = current->ai_next) {
        socket_t fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd == invalid_socket) {
            last_error = last_socket_error();
            continue;
        }

        try {
            set_reuse_addr(fd);
            try_enable_dual_stack(fd, current->ai_family);

            if (::bind(fd, current->ai_addr, static_cast<int>(current->ai_addrlen)) != 0) {
                last_error = last_socket_error();
                close_socket(fd);
                continue;
            }

            if (::listen(fd, backlog) != 0) {
                last_error = last_socket_error();
                close_socket(fd);
                continue;
            }

            set_non_blocking(fd);
            return fd;
        } catch (...) {
            close_socket(fd);
            throw;
        }
    }

    if (last_error) {
        throw std::system_error(last_error, "failed to create listening socket");
    }
    throw std::runtime_error("failed to create listening socket");
}

} // namespace cpp20_server::net
