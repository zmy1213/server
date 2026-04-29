#include "cpp20_server/net/tcp_server.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <system_error>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace cpp20_server::net {

namespace {

constexpr std::size_t read_buffer_size = 64 * 1024;

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

} // namespace

TcpServer::TcpServer(TcpServerOptions options)
    : options_(std::move(options)),
      poller_(options_.max_events),
      on_message_([](std::string_view message) { return std::string{message}; }) {}

TcpServer::~TcpServer() {
    for (auto& [_, connection] : connections_) {
        close_socket(connection.fd);
    }
    connections_.clear();
    close_socket(listen_fd_);
}

void TcpServer::set_message_callback(MessageCallback callback) {
    on_message_ = std::move(callback);
}

void TcpServer::start() {
    listen_fd_ = create_listening_socket(options_.host, options_.port, options_.backlog);
    poller_.add(listen_fd_, Event::read);

    std::cout << "listening on " << options_.host << ':' << options_.port
              << " backend=" << backend_name() << '\n';

    while (!stopping_.load()) {
        auto events = poller_.wait(std::chrono::milliseconds{1000});
        for (const auto& event : events) {
            if (event.fd == listen_fd_) {
                handle_accept();
                continue;
            }

            if (has_event(event.events, Event::read)) {
                handle_read(event.fd);
            }

            if (connections_.contains(event.fd) && has_event(event.events, Event::write)) {
                handle_write(event.fd);
            }

            if (connections_.contains(event.fd)
                && (has_event(event.events, Event::error) || has_event(event.events, Event::close))) {
                auto& connection = connections_.at(event.fd);
                if (connection.output.empty()) {
                    close_connection(event.fd);
                } else {
                    connection.close_after_write = true;
                    enable_write(connection);
                }
            }
        }
    }
}

void TcpServer::stop() noexcept {
    stopping_.store(true);
}

const TcpServerOptions& TcpServer::options() const noexcept {
    return options_;
}

const ServerStats& TcpServer::stats() const noexcept {
    return stats_;
}

const char* TcpServer::backend_name() const noexcept {
    return poller_.backend_name();
}

void TcpServer::handle_accept() {
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
            poller_.add(client, Event::read);
            connections_.emplace(client, Connection{client, Buffer{}, false});
            ++stats_.accepted_connections;
            stats_.active_connections = static_cast<std::uint64_t>(connections_.size());
        } catch (...) {
            close_socket(client);
            throw;
        }
    }
}

void TcpServer::handle_read(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    std::array<char, read_buffer_size> buffer{};
    for (;;) {
        const auto n = ::recv(fd, buffer.data(), safe_io_size(buffer.size()), 0);
        if (n > 0) {
            stats_.bytes_read += static_cast<std::uint64_t>(n);
            std::string response = on_message_(std::string_view{buffer.data(), static_cast<std::size_t>(n)});
            if (!response.empty()) {
                it->second.output.append(response);
            }
            continue;
        }

        if (n == 0) {
            if (it->second.output.empty()) {
                close_connection(fd);
            } else {
                it->second.close_after_write = true;
                enable_write(it->second);
            }
            return;
        }

        auto error = last_socket_error();
        if (is_interrupted(error)) {
            continue;
        }
        if (is_would_block(error)) {
            break;
        }

        close_connection(fd);
        return;
    }

    if (connections_.contains(fd) && !it->second.output.empty()) {
        enable_write(it->second);
    }
}

void TcpServer::handle_write(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    auto& connection = it->second;
    while (!connection.output.empty()) {
        const std::string_view data = connection.output.readable_view();
        const auto n = ::send(fd,
                              data.data(),
                              safe_io_size(data.size()),
                              send_flags());
        if (n > 0) {
            connection.output.retrieve(static_cast<std::size_t>(n));
            stats_.bytes_written += static_cast<std::uint64_t>(n);
            continue;
        }

        auto error = last_socket_error();
        if (is_interrupted(error)) {
            continue;
        }
        if (is_would_block(error)) {
            break;
        }

        close_connection(fd);
        return;
    }

    if (connections_.contains(fd)) {
        if (connection.output.empty()) {
            if (connection.close_after_write) {
                close_connection(fd);
            } else {
                disable_write(connection);
            }
        } else {
            enable_write(connection);
        }
    }
}

void TcpServer::close_connection(socket_t fd) noexcept {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    try {
        poller_.remove(fd);
    } catch (...) {
    }
    close_socket(fd);
    connections_.erase(it);
    stats_.active_connections = static_cast<std::uint64_t>(connections_.size());
}

void TcpServer::enable_write(Connection& connection) {
    poller_.modify(connection.fd, Event::read | Event::write);
}

void TcpServer::disable_write(Connection& connection) {
    poller_.modify(connection.fd, Event::read);
}

} // namespace cpp20_server::net
