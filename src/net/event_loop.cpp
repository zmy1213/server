#include "cpp20_server/net/event_loop.h"

#include "cpp20_server/net/channel.h"

#include <array>
#include <chrono>
#include <system_error>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace cpp20_server::net {

namespace {

struct WakeupSockets {
    socket_t read_fd{invalid_socket};
    socket_t write_fd{invalid_socket};
};

void close_wakeup_sockets(WakeupSockets sockets) noexcept {
    close_socket(sockets.read_fd);
    if (sockets.write_fd != sockets.read_fd) {
        close_socket(sockets.write_fd);
    }
}

int wakeup_send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void set_wakeup_no_sigpipe(socket_t fd) noexcept {
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
    (void)fd;
#endif
}

#if defined(_WIN32)

WakeupSockets create_wakeup_sockets() {
    socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == invalid_socket) {
        throw std::system_error(last_socket_error(), "wakeup listener socket failed");
    }

    WakeupSockets result;
    try {
        set_reuse_addr(listener);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::system_error(last_socket_error(), "wakeup listener bind failed");
        }
        if (::listen(listener, 1) != 0) {
            throw std::system_error(last_socket_error(), "wakeup listener listen failed");
        }

        int length = sizeof(addr);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
            throw std::system_error(last_socket_error(), "wakeup listener getsockname failed");
        }

        result.write_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (result.write_fd == invalid_socket) {
            throw std::system_error(last_socket_error(), "wakeup writer socket failed");
        }
        if (::connect(result.write_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::system_error(last_socket_error(), "wakeup writer connect failed");
        }

        result.read_fd = ::accept(listener, nullptr, nullptr);
        if (result.read_fd == invalid_socket) {
            throw std::system_error(last_socket_error(), "wakeup reader accept failed");
        }

        set_non_blocking(result.read_fd);
        set_non_blocking(result.write_fd);
        set_wakeup_no_sigpipe(result.write_fd);
        close_socket(listener);
        return result;
    } catch (...) {
        close_socket(listener);
        close_wakeup_sockets(result);
        throw;
    }
}

#else

WakeupSockets create_wakeup_sockets() {
    int fds[2]{invalid_socket, invalid_socket};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::system_error(last_socket_error(), "socketpair failed");
    }

    WakeupSockets result{fds[0], fds[1]};
    try {
        set_non_blocking(result.read_fd);
        set_non_blocking(result.write_fd);
        set_wakeup_no_sigpipe(result.write_fd);
        return result;
    } catch (...) {
        close_wakeup_sockets(result);
        throw;
    }
}

#endif

} // namespace

EventLoop::EventLoop(std::size_t max_events) : poller_(max_events) {
    const auto sockets = create_wakeup_sockets();
    wakeup_read_fd_ = sockets.read_fd;
    wakeup_write_fd_ = sockets.write_fd;

    wakeup_channel_ = std::make_unique<Channel>(*this, wakeup_read_fd_);
    wakeup_channel_->set_read_callback([this] {
        drain_wakeup();
    });
    wakeup_channel_->enable_reading();
}

EventLoop::~EventLoop() {
    if (wakeup_channel_) {
        try {
            wakeup_channel_->remove();
        } catch (...) {
        }
        wakeup_channel_.reset();
    }
    close_socket(wakeup_read_fd_);
    close_socket(wakeup_write_fd_);
}

void EventLoop::loop() {
    while (!stopping_.load()) {
        process_pending_tasks();
        timer_queue_.run_due_timers();

        const auto timeout = timer_queue_.next_timeout(std::chrono::milliseconds{50});
        auto fired_events = poller_.wait(timeout);
        for (const auto& fired : fired_events) {
            auto it = channels_.find(fired.fd);
            if (it == channels_.end() || it->second == nullptr) {
                continue;
            }
            it->second->handle_event(fired.events);
        }

        process_pending_tasks();
        timer_queue_.run_due_timers();
    }

    process_pending_tasks();
    timer_queue_.run_due_timers();
}

void EventLoop::stop() noexcept {
    stopping_.store(true);
    wakeup();
}

void EventLoop::run_in_loop(Task task) {
    {
        std::scoped_lock lock(pending_mutex_);
        pending_tasks_.push_back(std::move(task));
    }
    wakeup();
}

EventLoop::TimerId EventLoop::run_after(std::chrono::milliseconds delay, Task task) {
    const auto id = timer_queue_.run_after(delay, std::move(task));
    wakeup();
    return id;
}

EventLoop::TimerId EventLoop::run_every(std::chrono::milliseconds interval, Task task) {
    const auto id = timer_queue_.run_every(interval, std::move(task));
    wakeup();
    return id;
}

void EventLoop::cancel_timer(TimerId id) {
    timer_queue_.cancel(id);
    wakeup();
}

void EventLoop::update_channel(Channel& channel) {
    const socket_t fd = channel.fd();
    if (!channel.is_registered()) {
        poller_.add(fd, channel.events());
        channels_[fd] = &channel;
        channel.mark_registered(true);
        return;
    }

    poller_.modify(fd, channel.events());
    channels_[fd] = &channel;
}

void EventLoop::remove_channel(Channel& channel) {
    if (!channel.is_registered()) {
        return;
    }
    poller_.remove(channel.fd());
    channels_.erase(channel.fd());
    channel.mark_registered(false);
}

const char* EventLoop::backend_name() const noexcept {
    return poller_.backend_name();
}

void EventLoop::wakeup() noexcept {
    if (wakeup_write_fd_ == invalid_socket) {
        return;
    }

    const char byte = 1;
    const auto n = ::send(wakeup_write_fd_, &byte, 1, wakeup_send_flags());
    if (n < 0) {
        const auto error = last_socket_error();
        if (is_would_block(error) || is_interrupted(error)) {
            return;
        }
    }
}

void EventLoop::drain_wakeup() noexcept {
    std::array<char, 256> buffer{};
    for (;;) {
        const auto n = ::recv(wakeup_read_fd_, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (n > 0) {
            continue;
        }
        if (n == 0) {
            return;
        }

        const auto error = last_socket_error();
        if (is_interrupted(error)) {
            continue;
        }
        if (is_would_block(error)) {
            return;
        }
        return;
    }
}

void EventLoop::process_pending_tasks() {
    std::vector<Task> tasks;
    {
        std::scoped_lock lock(pending_mutex_);
        tasks.swap(pending_tasks_);
    }

    for (auto& task : tasks) {
        if (task) {
            task();
        }
    }
}

} // namespace cpp20_server::net
