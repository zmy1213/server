#include "cpp20_server/net/tcp_server.h"

#include "cpp20_server/net/socket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
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
#include "cpp20_server/net/acceptor.h"
#include "cpp20_server/net/connection.h"
#include "cpp20_server/net/event_loop.h"
#endif

namespace cpp20_server::net {

namespace {

#if defined(CPP20_SERVER_USE_IOCP)
constexpr std::size_t read_buffer_size = 64 * 1024;
#endif

std::chrono::milliseconds idle_timeout(const TcpServerOptions& options) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::seconds{options.idle_timeout_seconds});
}

TcpServer::StreamCallback make_stream_callback(TcpServer::MessageCallback callback) {
    return [callback = std::move(callback)](Buffer& input, Buffer& output) {
        const auto message = input.readable_view();
        if (message.empty()) {
            return;
        }
        std::string response = callback(message);
        input.retrieve(message.size());
        if (!response.empty()) {
            output.append(response);
        }
    };
}

#if defined(CPP20_SERVER_USE_IOCP)
std::int64_t steady_now_milliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
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
          on_stream_(make_stream_callback([](std::string_view message) { return std::string{message}; })) {}

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
        on_stream_ = make_stream_callback(std::move(callback));
    }

    void set_stream_callback(StreamCallback callback) {
        on_stream_ = std::move(callback);
    }

    void start() {
        listen_fd_ = create_listening_socket(options_.host, options_.port, options_.backlog);
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (iocp_ == nullptr) {
            throw std::system_error(GetLastError(), std::system_category(), "CreateIoCompletionPort failed");
        }

        start_workers();
        start_idle_cleaner();
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

    [[nodiscard]] ServerStats stats() const {
        std::scoped_lock lock(stats_mutex_);
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
        Buffer input{};
        Buffer output{};
        std::atomic_int pending_operations{0};
        std::atomic_bool closing{false};
        std::atomic_bool socket_closed{false};
        std::atomic<std::int64_t> last_active_ms{0};
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
        if (idle_cleaner_thread_.joinable()) {
            idle_cleaner_thread_.join();
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
        connection->last_active_ms.store(steady_now_milliseconds(), std::memory_order_relaxed);
        auto* raw = connection.get();

        HANDLE associated = CreateIoCompletionPort(reinterpret_cast<HANDLE>(client),
                                                   iocp_,
                                                   reinterpret_cast<ULONG_PTR>(raw),
                                                   0);
        if (associated == nullptr) {
            throw std::system_error(GetLastError(), std::system_category(), "CreateIoCompletionPort(socket) failed");
        }

        std::uint64_t active_connections = 0;
        {
            std::scoped_lock lock(connections_mutex_);
            connections_.emplace(client, std::move(connection));
            active_connections = static_cast<std::uint64_t>(connections_.size());
        }
        {
            std::scoped_lock lock(stats_mutex_);
            ++stats_.accepted_connections;
            stats_.active_connections = active_connections;
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

    void post_send(Connection* connection) {
        if (connection == nullptr || connection->closing.load() || connection->output.empty()) {
            return;
        }

        auto& context = connection->send_context;
        context = IoContext{};
        context.operation = IoOperation::send;
        const auto data = connection->output.readable_view();
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

            const bool connection_closed = !ok || transferred == 0 || connection->socket_closed.load();
            if (connection_closed) {
                close_connection(connection);
                finish_operation(connection);
                continue;
            }

            if (context->operation == IoOperation::recv) {
                connection->last_active_ms.store(steady_now_milliseconds(), std::memory_order_relaxed);
                {
                    std::scoped_lock lock(stats_mutex_);
                    stats_.bytes_read += transferred;
                }
                connection->input.append(
                    std::string_view{context->storage.data(), static_cast<std::size_t>(transferred)});
                if (on_stream_) {
                    on_stream_(connection->input, connection->output);
                }
                finish_operation(connection);
                if (!connection->output.empty()) {
                    post_send(connection);
                } else {
                    post_recv(connection);
                }
                continue;
            }

            if (context->operation == IoOperation::send) {
                connection->last_active_ms.store(steady_now_milliseconds(), std::memory_order_relaxed);
                connection->output.retrieve(static_cast<std::size_t>(transferred));
                {
                    std::scoped_lock lock(stats_mutex_);
                    stats_.bytes_written += transferred;
                }
                finish_operation(connection);
                if (!connection->output.empty()) {
                    post_send(connection);
                } else {
                    post_recv(connection);
                }
            }
        }
    }

    void start_idle_cleaner() {
        if (options_.idle_timeout_seconds == 0) {
            return;
        }
        idle_cleaner_thread_ = std::thread([this] { idle_cleaner_loop(); });
    }

    void idle_cleaner_loop() {
        while (!stopping_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            close_idle_connections();
        }
    }

    void close_idle_connections() noexcept {
        const auto timeout = idle_timeout(options_);
        if (timeout <= std::chrono::milliseconds{0}) {
            return;
        }

        const auto timeout_ms = timeout.count();
        const auto now_ms = steady_now_milliseconds();

        std::scoped_lock lock(connections_mutex_);
        for (auto& [_, connection] : connections_) {
            if (!connection || connection->closing.load()) {
                continue;
            }
            const auto last_active = connection->last_active_ms.load(std::memory_order_relaxed);
            if (now_ms - last_active < timeout_ms) {
                continue;
            }
            if (connection->socket_closed.load()) {
                continue;
            }
            close_connection_socket(connection.get());
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
            close_connection_socket(connection);
        }
        destroy_if_idle(connection);
    }

    void close_connection_socket(Connection* connection) noexcept {
        if (connection == nullptr) {
            return;
        }
        const bool was_closed = connection->socket_closed.exchange(true);
        if (!was_closed) {
            close_socket(connection->fd);
        }
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
            const auto active_connections = static_cast<std::uint64_t>(connections_.size());
            std::scoped_lock stats_lock(stats_mutex_);
            stats_.active_connections = active_connections;
        }
    }

    void close_all_connections() noexcept {
        std::scoped_lock lock(connections_mutex_);
        for (auto& [_, connection] : connections_) {
            close_connection_socket(connection.get());
        }
        connections_.clear();
        std::scoped_lock stats_lock(stats_mutex_);
        stats_.active_connections = 0;
    }

    SocketRuntime runtime_;
    TcpServerOptions options_;
    socket_t listen_fd_{invalid_socket};
    HANDLE iocp_{nullptr};
    StreamCallback on_stream_;
    ServerStats stats_;
    mutable std::mutex stats_mutex_;
    std::atomic_bool stopping_{false};
    std::size_t worker_count_{0};
    std::thread accept_thread_;
    std::thread idle_cleaner_thread_;
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
          accept_loop_(options_.max_events),
          on_stream_(make_stream_callback([](std::string_view message) { return std::string{message}; })) {}

    ~Impl() {
        stop();
        join_worker_loops();
        close_all_connections();
        acceptor_.reset();
    }

    void set_message_callback(MessageCallback callback) {
        on_stream_ = make_stream_callback(std::move(callback));
    }

    void set_stream_callback(StreamCallback callback) {
        on_stream_ = std::move(callback);
    }

    void start() {
        start_worker_loops();

        acceptor_ = std::make_unique<Acceptor>(accept_loop_, options_.host, options_.port, options_.backlog);
        acceptor_->set_new_connection_callback([this](socket_t client_fd) {
            handle_new_connection(client_fd);
        });

        std::cout << "listening on " << options_.host << ':' << options_.port
                  << " backend=" << backend_name()
                  << " worker_threads=" << worker_loops_.size() << '\n';

        accept_loop_.loop();
        join_worker_loops();
    }

    void stop() noexcept {
        accept_loop_.stop();
        for (auto& loop : worker_loops_) {
            if (loop) {
                loop->stop();
            }
        }
    }

    [[nodiscard]] const TcpServerOptions& options() const noexcept {
        return options_;
    }

    [[nodiscard]] ServerStats stats() const {
        std::scoped_lock lock(stats_mutex_);
        return stats_;
    }

    [[nodiscard]] const char* backend_name() const noexcept {
        return accept_loop_.backend_name();
    }

private:
    void start_worker_loops() {
        if (!worker_loops_.empty()) {
            return;
        }

        const auto hardware_threads = std::thread::hardware_concurrency();
        const std::size_t worker_count = options_.worker_threads == 0
                                             ? static_cast<std::size_t>(std::max(1U, hardware_threads))
                                             : options_.worker_threads;

        worker_loops_.reserve(worker_count);
        worker_threads_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            auto loop = std::make_unique<EventLoop>(options_.max_events);
            auto* raw_loop = loop.get();
            worker_loops_.push_back(std::move(loop));
            worker_threads_.emplace_back([raw_loop] {
                raw_loop->loop();
            });
        }
    }

    void join_worker_loops() {
        for (auto& thread : worker_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        worker_threads_.clear();
    }

    EventLoop& next_worker_loop() {
        auto& loop = *worker_loops_[next_worker_index_ % worker_loops_.size()];
        ++next_worker_index_;
        return loop;
    }

    void handle_new_connection(socket_t client_fd) {
        auto& target_loop = next_worker_loop();
        target_loop.run_in_loop([this, &target_loop, client_fd] {
            create_connection(target_loop, client_fd);
        });
    }

    void create_connection(EventLoop& loop, socket_t client_fd) {
        auto connection = std::make_unique<Connection>(loop, client_fd);
        connection->set_stream_callback(on_stream_);
        connection->set_bytes_read_callback([this](std::size_t bytes) {
            std::scoped_lock lock(stats_mutex_);
            stats_.bytes_read += static_cast<std::uint64_t>(bytes);
        });
        connection->set_bytes_written_callback([this](std::size_t bytes) {
            std::scoped_lock lock(stats_mutex_);
            stats_.bytes_written += static_cast<std::uint64_t>(bytes);
        });
        connection->set_close_callback([this](socket_t fd) {
            close_connection(fd);
        });

        auto* raw = connection.get();
        std::uint64_t active_connections = 0;
        {
            std::scoped_lock lock(connections_mutex_);
            connections_.emplace(client_fd, std::move(connection));
            active_connections = static_cast<std::uint64_t>(connections_.size());
        }
        {
            std::scoped_lock lock(stats_mutex_);
            ++stats_.accepted_connections;
            stats_.active_connections = active_connections;
        }
        raw->start();
        schedule_idle_check(loop, client_fd);
    }

    void schedule_idle_check(EventLoop& loop, socket_t fd) {
        const auto timeout = idle_timeout(options_);
        if (timeout <= std::chrono::milliseconds{0}) {
            return;
        }

        loop.run_after(timeout, [this, &loop, fd] {
            check_idle_connection(loop, fd);
        });
    }

    void check_idle_connection(EventLoop& loop, socket_t fd) {
        const auto timeout = idle_timeout(options_);
        if (timeout <= std::chrono::milliseconds{0}) {
            return;
        }

        std::chrono::milliseconds idle{0};
        {
            std::scoped_lock lock(connections_mutex_);
            auto it = connections_.find(fd);
            if (it == connections_.end()) {
                return;
            }
            idle = it->second->idle_for(Connection::Clock::now());
        }

        if (idle >= timeout) {
            close_connection(fd);
            return;
        }

        auto remaining = timeout - idle;
        if (remaining <= std::chrono::milliseconds{0}) {
            remaining = std::chrono::milliseconds{1};
        }
        loop.run_after(remaining, [this, &loop, fd] {
            check_idle_connection(loop, fd);
        });
    }

    void close_connection(socket_t fd) noexcept {
        std::unique_ptr<Connection> connection;
        {
            std::scoped_lock lock(connections_mutex_);
            auto it = connections_.find(fd);
            if (it == connections_.end()) {
                return;
            }
            connection = std::move(it->second);
            connections_.erase(it);
            const auto active_connections = static_cast<std::uint64_t>(connections_.size());
            std::scoped_lock stats_lock(stats_mutex_);
            stats_.active_connections = active_connections;
        }

        connection->close();
    }

    void close_all_connections() noexcept {
        std::unordered_map<socket_t, std::unique_ptr<Connection>> connections;
        {
            std::scoped_lock lock(connections_mutex_);
            connections.swap(connections_);
        }
        {
            std::scoped_lock lock(stats_mutex_);
            stats_.active_connections = 0;
        }

        for (auto& [_, connection] : connections) {
            connection->close();
        }
    }

    SocketRuntime runtime_;
    TcpServerOptions options_;
    EventLoop accept_loop_;
    std::unique_ptr<Acceptor> acceptor_;
    std::vector<std::unique_ptr<EventLoop>> worker_loops_;
    std::vector<std::thread> worker_threads_;
    std::size_t next_worker_index_{0};
    std::mutex connections_mutex_;
    mutable std::mutex stats_mutex_;
    std::unordered_map<socket_t, std::unique_ptr<Connection>> connections_;
    StreamCallback on_stream_;
    ServerStats stats_;
};

#endif

TcpServer::TcpServer(TcpServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

TcpServer::~TcpServer() = default;

void TcpServer::set_message_callback(MessageCallback callback) {
    impl_->set_message_callback(std::move(callback));
}

void TcpServer::set_stream_callback(StreamCallback callback) {
    impl_->set_stream_callback(std::move(callback));
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

ServerStats TcpServer::stats() const {
    return impl_->stats();
}

const char* TcpServer::backend_name() const noexcept {
    return impl_->backend_name();
}

} // namespace cpp20_server::net
