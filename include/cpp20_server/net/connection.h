#pragma once

#include "cpp20_server/net/buffer.h"
#include "cpp20_server/net/channel.h"
#include "cpp20_server/net/socket.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace cpp20_server::net {

class EventLoop;

// Connection owns one client socket and its read/write state.
class Connection {
public:
    using MessageCallback = std::function<std::string(std::string_view message)>;
    using CloseCallback = std::function<void(socket_t fd)>;
    using BytesCallback = std::function<void(std::size_t bytes)>;

    Connection(EventLoop& loop, socket_t fd);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void set_message_callback(MessageCallback callback);
    void set_close_callback(CloseCallback callback);
    void set_bytes_read_callback(BytesCallback callback);
    void set_bytes_written_callback(BytesCallback callback);

    void start();
    void close() noexcept;

    [[nodiscard]] socket_t fd() const noexcept;

private:
    void handle_read();
    void handle_write();
    void handle_peer_close();
    void handle_error();
    void enable_write();
    void disable_write();

    EventLoop& loop_;
    socket_t fd_{invalid_socket};
    Channel channel_;
    Buffer output_;
    bool close_after_write_{false};
    MessageCallback message_callback_;
    CloseCallback close_callback_;
    BytesCallback bytes_read_callback_;
    BytesCallback bytes_written_callback_;
};

} // namespace cpp20_server::net
