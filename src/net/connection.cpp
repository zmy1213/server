#include "cpp20_server/net/connection.h"

#include "cpp20_server/net/event_loop.h"

#include <algorithm>
#include <array>
#include <limits>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

namespace cpp20_server::net {

namespace {

constexpr std::size_t read_buffer_size = 64 * 1024;

int safe_io_size(std::size_t value) noexcept {
    const auto max_value = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(value, max_value));
}

int send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

} // namespace

Connection::Connection(EventLoop& loop, socket_t fd)
    : loop_(loop),
      fd_(fd),
      channel_(loop_, fd_),
      message_callback_([](std::string_view message) { return std::string{message}; }) {
    channel_.set_read_callback([this] { handle_read(); });
    channel_.set_write_callback([this] { handle_write(); });
    channel_.set_close_callback([this] { handle_peer_close(); });
    channel_.set_error_callback([this] { handle_error(); });
}

Connection::~Connection() {
    close();
}

void Connection::set_message_callback(MessageCallback callback) {
    message_callback_ = std::move(callback);
}

void Connection::set_close_callback(CloseCallback callback) {
    close_callback_ = std::move(callback);
}

void Connection::set_bytes_read_callback(BytesCallback callback) {
    bytes_read_callback_ = std::move(callback);
}

void Connection::set_bytes_written_callback(BytesCallback callback) {
    bytes_written_callback_ = std::move(callback);
}

void Connection::start() {
    touch();
    channel_.enable_reading();
}

void Connection::close() noexcept {
    if (fd_ == invalid_socket) {
        return;
    }

    try {
        channel_.remove();
    } catch (...) {
    }
    close_socket(fd_);
    fd_ = invalid_socket;
}

socket_t Connection::fd() const noexcept {
    return fd_;
}

std::chrono::milliseconds Connection::idle_for(Clock::time_point now) const noexcept {
    if (now <= last_active_at_) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - last_active_at_);
}

void Connection::touch() noexcept {
    last_active_at_ = Clock::now();
}

void Connection::handle_read() {
    std::array<char, read_buffer_size> buffer{};
    for (;;) {
        const auto n = ::recv(fd_, buffer.data(), safe_io_size(buffer.size()), 0);
        if (n > 0) {
            touch();
            if (bytes_read_callback_) {
                bytes_read_callback_(static_cast<std::size_t>(n));
            }
            std::string response = message_callback_(
                std::string_view{buffer.data(), static_cast<std::size_t>(n)});
            if (!response.empty()) {
                output_.append(response);
            }
            continue;
        }

        if (n == 0) {
            handle_peer_close();
            return;
        }

        auto error = last_socket_error();
        if (is_interrupted(error)) {
            continue;
        }
        if (is_would_block(error)) {
            break;
        }

        handle_error();
        return;
    }

    if (!output_.empty()) {
        enable_write();
    }
}

void Connection::handle_write() {
    while (!output_.empty()) {
        const std::string_view data = output_.readable_view();
        const auto n = ::send(fd_, data.data(), safe_io_size(data.size()), send_flags());
        if (n > 0) {
            touch();
            output_.retrieve(static_cast<std::size_t>(n));
            if (bytes_written_callback_) {
                bytes_written_callback_(static_cast<std::size_t>(n));
            }
            continue;
        }

        auto error = last_socket_error();
        if (is_interrupted(error)) {
            continue;
        }
        if (is_would_block(error)) {
            break;
        }

        handle_error();
        return;
    }

    if (output_.empty()) {
        if (close_after_write_) {
            if (close_callback_) {
                close_callback_(fd_);
            }
            return;
        }
        disable_write();
    } else {
        enable_write();
    }
}

void Connection::handle_peer_close() {
    if (output_.empty()) {
        if (close_callback_) {
            close_callback_(fd_);
        }
        return;
    }

    close_after_write_ = true;
    enable_write();
}

void Connection::handle_error() {
    if (close_callback_) {
        close_callback_(fd_);
    }
}

void Connection::enable_write() {
    channel_.enable_writing();
}

void Connection::disable_write() {
    channel_.disable_writing();
}

} // namespace cpp20_server::net
