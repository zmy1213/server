#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace cpp20_server::net {

struct TcpServerOptions {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    int backlog{4096};
    std::size_t max_events{4096};
    std::size_t worker_threads{0};
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
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cpp20_server::net
