#pragma once

#include "cpp20_server/net/channel.h"
#include "cpp20_server/net/socket.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace cpp20_server::net {

class EventLoop;

// Acceptor owns the listening socket and turns readable listen events into
// accepted client sockets.
class Acceptor {
public:
    using NewConnectionCallback = std::function<void(socket_t client_fd)>;

    Acceptor(EventLoop& loop, std::string_view host, std::uint16_t port, int backlog);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void set_new_connection_callback(NewConnectionCallback callback);

    [[nodiscard]] socket_t listen_fd() const noexcept;

private:
    void handle_read();

    EventLoop& loop_;
    socket_t listen_fd_{invalid_socket};
    Channel channel_;
    NewConnectionCallback new_connection_callback_;
};

} // namespace cpp20_server::net
