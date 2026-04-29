#pragma once

#include "cpp20_server/net/buffer.h"
#include "cpp20_server/net/poller.h"
#include "cpp20_server/net/socket.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cpp20_server::net {

struct TcpServerOptions {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    int backlog{4096};
    std::size_t max_events{4096};
};

struct ServerStats {
    std::uint64_t accepted_connections{0};
    std::uint64_t active_connections{0};
    std::uint64_t bytes_read{0};
    std::uint64_t bytes_written{0};
};

class TcpServer {
public:
    using MessageCallback = std::function<std::string(std::string_view message)>;

    explicit TcpServer(TcpServerOptions options);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void set_message_callback(MessageCallback callback);
    void start();
    void stop() noexcept;

    [[nodiscard]] const TcpServerOptions& options() const noexcept;
    [[nodiscard]] const ServerStats& stats() const noexcept;
    [[nodiscard]] const char* backend_name() const noexcept;

private:
    struct Connection {
        socket_t fd{invalid_socket};
        Buffer output;
        bool close_after_write{false};
    };

    void handle_accept();
    void handle_read(socket_t fd);
    void handle_write(socket_t fd);
    void close_connection(socket_t fd) noexcept;
    void enable_write(Connection& connection);
    void disable_write(Connection& connection);

    SocketRuntime runtime_;
    TcpServerOptions options_;
    Poller poller_;
    socket_t listen_fd_{invalid_socket};
    std::unordered_map<socket_t, Connection> connections_;
    MessageCallback on_message_;
    ServerStats stats_;
    std::atomic_bool stopping_{false};
};

} // namespace cpp20_server::net
