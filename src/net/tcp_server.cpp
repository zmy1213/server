#include "cpp20_server/net/tcp_server.h"

#include "cpp20_server/net/buffer.h"
#include "cpp20_server/net/socket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(CPP20_SERVER_USE_IOCP)
#include <mswsock.h>
#include <windows.h>
#else
#include "cpp20_server/net/poller.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#endif

namespace cpp20_server::net {

namespace {

constexpr std::size_t read_buffer_size = 64 * 1024;

int safe_io_size(std::size_t value) noexcept {
    const auto max_value = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(value, max_value));
}

#if !defined(CPP20_SERVER_USE_IOCP)
int send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
#endif

} // namespace

#if defined(CPP20_SERVER_USE_IOCP)

// Windows IOCP is not a "socket is readable/writable" API like epoll/kqueue.
// It is a "your async read/write has completed" API. That is why the Windows
// server has a separate implementation instead of pretending IOCP is Poller.
class TcpServer::Impl {
public:
    explicit Impl(TcpServerOptions options)
        : options_(std::move(options)),
          on_message_([](std::string_view message) { return std::string{message}; }) {}

    ~Impl() {
        stop();
        join_threads();
        close_all_connections();
        close_socket(listen_fd_);
        if (iocp_ != nullptr) {
            CloseHandle(iocp_);
        }
    }

    void set_message_callback(MessageCallback callback) {
        on_message_ = std::move(callback);
    }

    void start() {
        listen_fd_ = create_listening_socket(options_.host, options_.port, options_.backlog);
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (iocp_ == nullptr) {
            throw std::system_error(GetLastError(), std::system_category(), "CreateIoCompletionPort failed");
        }

        start_workers();
        accept_thread_ = std::thread([this] { accept_loop(); });

        std::cout << "listening on " << options_.host << ':' << options_.port
                  << " backend=" << backend_name()
                  << " worker_threads=" << worker_count_ << '\n';

        while (!stopping_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }

        join_threads();
    }

    void stop() noexcept {
        const bool was_stopping = stopping_.exchange(true);
        if (was_stopping) {
            return;
        }

        close_socket(listen_fd_);
        listen_fd_ = invalid_socket;

        if (iocp_ != nullptr) {
            for (std::size_t i = 0; i < worker_count_; ++i) {
                PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
            }
        }
    }

    [[nodiscard]] const TcpServerOptions& options() const noexcept {
        return options_;
    }

    [[nodiscard]] const ServerStats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] const char* backend_name() const noexcept {
        return CPP20_SERVER_BACKEND_NAME;
    }

private:
    enum class IoOperation {
        recv,
        send,
    };

    struct Connection;

    struct IoContext {
        OVERLAPPED overlapped{};
        WSABUF wsabuf{};
        IoOperation operation{IoOperation::recv};
        std::array<char, read_buffer_size> storage{};
    };

    struct Connection {
        socket_t fd{invalid_socket};
        IoContext recv_context{};
        IoContext send_context{};
        std::atomic_int pending_operations{0};
        std::atomic_bool closing{false};
    };

    void start_workers() {
        worker_count_ = options_.worker_threads == 0
                            ? std::max(1U, std::thread::hardware_concurrency())
                            : static_cast<unsigned int>(options_.worker_threads);
        worker_threads_.reserve(worker_count_);
        for (std::size_t i = 0; i < worker_count_; ++i) {
            worker_threads_.emplace_back([this] { worker_loop(); });
        }
    }

    void join_threads() {
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        for (auto& worker : worker_threads_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        worker_threads_.clear();
    }

    void accept_loop() {
        while (!stopping_.load()) {
            sockaddr_storage peer_addr{};
            int peer_len = sizeof(peer_addr);
            socket_t client = ::accept(listen_fd_,
                                       reinterpret_cast<sockaddr*>(&peer_addr),
                                       &peer_len);
            if (client == invalid_socket) {
                auto error = last_socket_error();
                if (stopping_.load()) {
                    return;
                }
                if (is_interrupted(error)) {
                    continue;
                }
                if (is_would_block(error)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    continue;
                }
                std::cerr << "accept failed: " << error.message() << '\n';
                continue;
            }

            try {
                set_no_delay(client);
                register_connection(client);
            } catch (const std::exception& ex) {
                std::cerr << "failed to register connection: " << ex.what() << '\n';
                close_socket(client);
            }
        }
    }

    void register_connection(socket_t client) {
        auto connection = std::make_unique<Connection>();
        connection->fd = client;
        auto* raw = connection.get();

        HANDLE associated = CreateIoCompletionPort(reinterpret_cast<HANDLE>(client),
                                                   iocp_,
                                                   reinterpret_cast<ULONG_PTR>(raw),
                                                   0);
        if (associated == nullptr) {
            throw std::system_error(GetLastError(), std::system_category(), "CreateIoCompletionPort(socket) failed");
        }

        {
            std::scoped_lock lock(connections_mutex_);
            connections_.emplace(client, std::move(connection));
            ++stats_.accepted_connections;
            stats_.active_connections = static_cast<std::uint64_t>(connections_.size());
        }

        post_recv(raw);
    }

    void post_recv(Connection* connection) {
        if (connection == nullptr || connection->closing.load()) {
            return;
        }

        auto& context = connection->recv_context;
        context = IoContext{};
        context.operation = IoOperation::recv;
        context.wsabuf.buf = context.storage.data();
        context.wsabuf.len = static_cast<ULONG>(context.storage.size());

        DWORD flags = 0;
        DWORD received = 0;
        connection->pending_operations.fetch_add(1);
        const int rc = WSARecv(connection->fd,
                               &context.wsabuf,
                               1,
                               &received,
                               &flags,
                               &context.overlapped,
                               nullptr);
        if (rc == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                connection->pending_operations.fetch_sub(1);
                close_connection(connection);
            }
        }
    }

    void post_send(Connection* connection, std::string_view data) {
        if (connection == nullptr || connection->closing.load() || data.empty()) {
            return;
        }

        auto& context = connection->send_context;
        context = IoContext{};
        context.operation = IoOperation::send;
        const std::size_t size = std::min(data.size(), context.storage.size());
        std::copy_n(data.data(), size, context.storage.data());
        context.wsabuf.buf = context.storage.data();
        context.wsabuf.len = static_cast<ULONG>(size);

        DWORD sent = 0;
        connection->pending_operations.fetch_add(1);
        const int rc = WSASend(connection->fd,
                               &context.wsabuf,
                               1,
                               &sent,
                               0,
                               &context.overlapped,
                               nullptr);
        if (rc == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                connection->pending_operations.fetch_sub(1);
                close_connection(connection);
            }
        }
    }

    void worker_loop() {
        while (!stopping_.load()) {
            DWORD transferred = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* overlapped = nullptr;
            const BOOL ok = GetQueuedCompletionStatus(iocp_,
                                                      &transferred,
                                                      &key,
                                                      &overlapped,
                                                      INFINITE);
            if (overlapped == nullptr && key == 0) {
                return;
            }

            auto* connection = reinterpret_cast<Connection*>(key);
            auto* context = reinterpret_cast<IoContext*>(overlapped);
            if (connection == nullptr || context == nullptr) {
                continue;
            }

            const bool connection_closed = !ok || transferred == 0;
            if (connection_closed) {
                finish_operation(connection);
                close_connection(connection);
                continue;
            }

            if (context->operation == IoOperation::recv) {
                {
                    std::scoped_lock lock(stats_mutex_);
                    stats_.bytes_read += transferred;
                }
                std::string response = on_message_(
                    std::string_view{context->storage.data(), static_cast<std::size_t>(transferred)});
                finish_operation(connection);
                post_send(connection, response);
                continue;
            }

            if (context->operation == IoOperation::send) {
                {
                    std::scoped_lock lock(stats_mutex_);
                    stats_.bytes_written += transferred;
                }
                finish_operation(connection);
                post_recv(connection);
            }
        }
    }

    void finish_operation(Connection* connection) {
        if (connection == nullptr) {
            return;
        }
        connection->pending_operations.fetch_sub(1);
        destroy_if_idle(connection);
    }

    void close_connection(Connection* connection) noexcept {
        if (connection == nullptr) {
            return;
        }

        const bool was_closing = connection->closing.exchange(true);
        if (!was_closing) {
            close_socket(connection->fd);
        }
        destroy_if_idle(connection);
    }

    void destroy_if_idle(Connection* connection) noexcept {
        if (connection == nullptr || !connection->closing.load()) {
            return;
        }
        if (connection->pending_operations.load() != 0) {
            return;
        }

        std::scoped_lock lock(connections_mutex_);
        auto it = connections_.find(connection->fd);
        if (it != connections_.end()) {
            connections_.erase(it);
            stats_.active_connections = static_cast<std::uint64_t>(connections_.size());
        }
    }

    void close_all_connections() noexcept {
        std::scoped_lock lock(connections_mutex_);
        for (auto& [_, connection] : connections_) {
            close_socket(connection->fd);
        }
        connections_.clear();
        stats_.active_connections = 0;
    }

    SocketRuntime runtime_;
    TcpServerOptions options_;
    socket_t listen_fd_{invalid_socket};
    HANDLE iocp_{nullptr};
    MessageCallback on_message_;
    ServerStats stats_;
    mutable std::mutex stats_mutex_;
    std::atomic_bool stopping_{false};
    std::size_t worker_count_{0};
    std::thread accept_thread_;
    std::vector<std::thread> worker_threads_;
    std::mutex connections_mutex_;
    std::unordered_map<socket_t, std::unique_ptr<Connection>> connections_;
};

#else

// Linux/macOS/select use the Reactor model:
// 1. Put sockets into the OS event queue.
// 2. Wait until the OS says "this socket can read/write now".
// 3. Call recv/send only when it should not block.
class TcpServer::Impl {
public:
    explicit Impl(TcpServerOptions options)
        : options_(std::move(options)),
          poller_(options_.max_events),
          on_message_([](std::string_view message) { return std::string{message}; }) {}

    ~Impl() {
        for (auto& [_, connection] : connections_) {
            close_socket(connection.fd);
        }
        connections_.clear();
        close_socket(listen_fd_);
    }

    void set_message_callback(MessageCallback callback) {
        on_message_ = std::move(callback);
    }

    void start() {
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

    void stop() noexcept {
        stopping_.store(true);
    }

    [[nodiscard]] const TcpServerOptions& options() const noexcept {
        return options_;
    }

    [[nodiscard]] const ServerStats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] const char* backend_name() const noexcept {
        return poller_.backend_name();
    }

private:
    struct Connection {
        socket_t fd{invalid_socket};
        Buffer output;
        bool close_after_write{false};
    };

    void handle_accept() {
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

    void handle_read(socket_t fd) {
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

    void handle_write(socket_t fd) {
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

    void close_connection(socket_t fd) noexcept {
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

    void enable_write(Connection& connection) {
        poller_.modify(connection.fd, Event::read | Event::write);
    }

    void disable_write(Connection& connection) {
        poller_.modify(connection.fd, Event::read);
    }

    SocketRuntime runtime_;
    TcpServerOptions options_;
    Poller poller_;
    socket_t listen_fd_{invalid_socket};
    std::unordered_map<socket_t, Connection> connections_;
    MessageCallback on_message_;
    ServerStats stats_;
    std::atomic_bool stopping_{false};
};

#endif

TcpServer::TcpServer(TcpServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

TcpServer::~TcpServer() = default;

void TcpServer::set_message_callback(MessageCallback callback) {
    impl_->set_message_callback(std::move(callback));
}

void TcpServer::start() {
    impl_->start();
}

void TcpServer::stop() noexcept {
    impl_->stop();
}

const TcpServerOptions& TcpServer::options() const noexcept {
    return impl_->options();
}

const ServerStats& TcpServer::stats() const noexcept {
    return impl_->stats();
}

const char* TcpServer::backend_name() const noexcept {
    return impl_->backend_name();
}

} // namespace cpp20_server::net
