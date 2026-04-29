#include "cpp20_server/net/acceptor.h"

#include "cpp20_server/net/event_loop.h"

#include <system_error>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace cpp20_server::net {

Acceptor::Acceptor(EventLoop& loop, std::string_view host, std::uint16_t port, int backlog)
    : loop_(loop),
      listen_fd_(create_listening_socket(host, port, backlog)),
      channel_(loop_, listen_fd_) {
    channel_.set_read_callback([this] { handle_read(); });
    channel_.enable_reading();
}

Acceptor::~Acceptor() {
    try {
        channel_.remove();
    } catch (...) {
    }
    close_socket(listen_fd_);
}

void Acceptor::set_new_connection_callback(NewConnectionCallback callback) {
    new_connection_callback_ = std::move(callback);
}

socket_t Acceptor::listen_fd() const noexcept {
    return listen_fd_;
}

void Acceptor::handle_read() {
    for (;;) {
        sockaddr_storage peer_addr{};
#if defined(_WIN32)
        int peer_len = sizeof(peer_addr);
#else
        socklen_t peer_len = sizeof(peer_addr);
#endif
        socket_t client = ::accept(listen_fd_,
                                   reinterpret_cast<sockaddr*>(&peer_addr),
                                   &peer_len);
        if (client == invalid_socket) {
            auto error = last_socket_error();
            if (is_would_block(error) || is_interrupted(error)) {
                return;
            }
            throw std::system_error(error, "accept failed");
        }

        try {
            set_non_blocking(client);
            set_no_delay(client);
            if (new_connection_callback_) {
                new_connection_callback_(client);
            } else {
                close_socket(client);
            }
        } catch (...) {
            close_socket(client);
            throw;
        }
    }
}

} // namespace cpp20_server::net
