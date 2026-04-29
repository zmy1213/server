#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#ifndef FD_SETSIZE
#define FD_SETSIZE 16384
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace cpp20_server::net {

#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t invalid_socket = -1;
#endif

class SocketRuntime {
public:
    SocketRuntime();
    ~SocketRuntime();

    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

[[nodiscard]] std::error_code last_socket_error() noexcept;
[[nodiscard]] bool is_would_block(std::error_code error) noexcept;
[[nodiscard]] bool is_interrupted(std::error_code error) noexcept;

void close_socket(socket_t fd) noexcept;
void set_non_blocking(socket_t fd);
void set_reuse_addr(socket_t fd);
void set_no_delay(socket_t fd);

[[nodiscard]] socket_t create_listening_socket(std::string_view host,
                                               std::uint16_t port,
                                               int backlog);

} // namespace cpp20_server::net
