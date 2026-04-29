#include "cpp20_server/net/poller.h"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#if defined(CPP20_SERVER_USE_EPOLL)
#include <sys/epoll.h>
#include <unistd.h>
#elif defined(CPP20_SERVER_USE_KQUEUE)
#include <sys/event.h>
#include <unistd.h>
#elif defined(CPP20_SERVER_USE_SELECT)
#if !defined(_WIN32)
#include <sys/select.h>
#endif
#endif

namespace cpp20_server::net {

namespace {

std::size_t normalize_max_events(std::size_t max_events) {
    return max_events == 0 ? 1 : max_events;
}

#if defined(CPP20_SERVER_USE_EPOLL)

std::uint32_t to_epoll_events(Event events) {
    std::uint32_t value = 0;
    if (has_event(events, Event::read)) {
        value |= EPOLLIN;
    }
    if (has_event(events, Event::write)) {
        value |= EPOLLOUT;
    }
    value |= EPOLLERR | EPOLLHUP;
#if defined(EPOLLRDHUP)
    value |= EPOLLRDHUP;
#endif
    return value;
}

Event from_epoll_events(std::uint32_t events) {
    Event value = Event::none;
    if ((events & EPOLLIN) != 0U) {
        value |= Event::read;
    }
    if ((events & EPOLLOUT) != 0U) {
        value |= Event::write;
    }
    if ((events & EPOLLERR) != 0U) {
        value |= Event::error;
    }
    if ((events & EPOLLHUP) != 0U) {
        value |= Event::close;
    }
#if defined(EPOLLRDHUP)
    if ((events & EPOLLRDHUP) != 0U) {
        value |= Event::close;
    }
#endif
    return value;
}

#endif

} // namespace

class Poller::Impl {
public:
    explicit Impl(std::size_t max_events)
#if defined(CPP20_SERVER_USE_EPOLL)
        : epoll_fd_(epoll_create1(EPOLL_CLOEXEC)),
          events_(normalize_max_events(max_events))
#elif defined(CPP20_SERVER_USE_KQUEUE)
        : kqueue_fd_(kqueue()),
          events_(normalize_max_events(max_events))
#else
        : max_events_(normalize_max_events(max_events))
#endif
    {
#if defined(CPP20_SERVER_USE_EPOLL)
        if (epoll_fd_ < 0) {
            throw std::system_error(last_socket_error(), "epoll_create1 failed");
        }
#elif defined(CPP20_SERVER_USE_KQUEUE)
        if (kqueue_fd_ < 0) {
            throw std::system_error(last_socket_error(), "kqueue failed");
        }
#endif
    }

    ~Impl() {
#if defined(CPP20_SERVER_USE_EPOLL)
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
        }
#elif defined(CPP20_SERVER_USE_KQUEUE)
        if (kqueue_fd_ >= 0) {
            close(kqueue_fd_);
        }
#endif
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void add(socket_t fd, Event events) {
#if defined(CPP20_SERVER_USE_EPOLL)
        epoll_event event{};
        event.events = to_epoll_events(events);
        event.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
            throw std::system_error(last_socket_error(), "epoll_ctl(ADD) failed");
        }
#elif defined(CPP20_SERVER_USE_KQUEUE)
        apply_kqueue_events(fd, events);
#else
        check_select_fd(fd);
        interests_[fd] = events;
#endif
    }

    void modify(socket_t fd, Event events) {
#if defined(CPP20_SERVER_USE_EPOLL)
        epoll_event event{};
        event.events = to_epoll_events(events);
        event.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
            throw std::system_error(last_socket_error(), "epoll_ctl(MOD) failed");
        }
#elif defined(CPP20_SERVER_USE_KQUEUE)
        apply_kqueue_events(fd, events);
#else
        check_select_fd(fd);
        interests_[fd] = events;
#endif
    }

    void remove(socket_t fd) {
#if defined(CPP20_SERVER_USE_EPOLL)
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
            auto error = last_socket_error();
            if (error.value() != EBADF && error.value() != ENOENT) {
                throw std::system_error(error, "epoll_ctl(DEL) failed");
            }
        }
#elif defined(CPP20_SERVER_USE_KQUEUE)
        delete_kqueue_filter(fd, EVFILT_READ);
        delete_kqueue_filter(fd, EVFILT_WRITE);
#else
        interests_.erase(fd);
#endif
    }

    std::vector<FiredEvent> wait(std::chrono::milliseconds timeout) {
#if defined(CPP20_SERVER_USE_EPOLL)
        const int timeout_ms = static_cast<int>(timeout.count());
        const int count = epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
        if (count < 0) {
            auto error = last_socket_error();
            if (is_interrupted(error)) {
                return {};
            }
            throw std::system_error(error, "epoll_wait failed");
        }

        std::vector<FiredEvent> fired;
        fired.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            fired.push_back(FiredEvent{events_[i].data.fd, from_epoll_events(events_[i].events)});
        }
        return fired;
#elif defined(CPP20_SERVER_USE_KQUEUE)
        timespec ts{};
        ts.tv_sec = timeout.count() / 1000;
        ts.tv_nsec = (timeout.count() % 1000) * 1000 * 1000;

        const int count = kevent(kqueue_fd_,
                                 nullptr,
                                 0,
                                 events_.data(),
                                 static_cast<int>(events_.size()),
                                 &ts);
        if (count < 0) {
            auto error = last_socket_error();
            if (is_interrupted(error)) {
                return {};
            }
            throw std::system_error(error, "kevent wait failed");
        }

        std::vector<FiredEvent> fired;
        fired.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            Event event = Event::none;
            if (events_[i].filter == EVFILT_READ) {
                event |= Event::read;
            }
            if (events_[i].filter == EVFILT_WRITE) {
                event |= Event::write;
            }
            if ((events_[i].flags & EV_EOF) != 0) {
                event |= Event::close;
            }
            if ((events_[i].flags & EV_ERROR) != 0) {
                event |= Event::error;
            }
            fired.push_back(FiredEvent{static_cast<socket_t>(events_[i].ident), event});
        }
        return fired;
#else
        return wait_select(timeout);
#endif
    }

    [[nodiscard]] const char* backend_name() const noexcept {
        return CPP20_SERVER_BACKEND_NAME;
    }

private:
#if defined(CPP20_SERVER_USE_KQUEUE)
    void apply_kqueue_events(socket_t fd, Event events) {
        delete_kqueue_filter(fd, EVFILT_READ);
        delete_kqueue_filter(fd, EVFILT_WRITE);

        if (has_event(events, Event::read)) {
            add_kqueue_filter(fd, EVFILT_READ);
        }
        if (has_event(events, Event::write)) {
            add_kqueue_filter(fd, EVFILT_WRITE);
        }
    }

    void add_kqueue_filter(socket_t fd, short filter) {
        struct kevent change{};
        EV_SET(&change, static_cast<uintptr_t>(fd), filter, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr) < 0) {
            throw std::system_error(last_socket_error(), "kevent add failed");
        }
    }

    void delete_kqueue_filter(socket_t fd, short filter) noexcept {
        struct kevent change{};
        EV_SET(&change, static_cast<uintptr_t>(fd), filter, EV_DELETE, 0, 0, nullptr);
        kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr);
    }
#endif

#if defined(CPP20_SERVER_USE_SELECT)
    void check_select_fd(socket_t fd) const {
#if !defined(_WIN32)
        if (fd >= FD_SETSIZE) {
            throw std::runtime_error("select backend fd exceeds FD_SETSIZE");
        }
#else
        (void)fd;
#endif
    }

    std::vector<FiredEvent> wait_select(std::chrono::milliseconds timeout) {
        fd_set read_set;
        fd_set write_set;
        fd_set except_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_ZERO(&except_set);

        socket_t max_fd = 0;
        std::size_t watched = 0;
        for (const auto& [fd, events] : interests_) {
            if (watched >= max_events_) {
                break;
            }
            if (has_event(events, Event::read)) {
                FD_SET(fd, &read_set);
            }
            if (has_event(events, Event::write)) {
                FD_SET(fd, &write_set);
            }
            FD_SET(fd, &except_set);
            if (fd > max_fd) {
                max_fd = fd;
            }
            ++watched;
        }

        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

#if defined(_WIN32)
        const int ready = select(0, &read_set, &write_set, &except_set, &tv);
#else
        const int ready = select(max_fd + 1, &read_set, &write_set, &except_set, &tv);
#endif
        if (ready < 0) {
            auto error = last_socket_error();
            if (is_interrupted(error)) {
                return {};
            }
            throw std::system_error(error, "select failed");
        }

        std::vector<FiredEvent> fired;
        fired.reserve(static_cast<std::size_t>(ready));
        for (const auto& [fd, _] : interests_) {
            Event event = Event::none;
            if (FD_ISSET(fd, &read_set)) {
                event |= Event::read;
            }
            if (FD_ISSET(fd, &write_set)) {
                event |= Event::write;
            }
            if (FD_ISSET(fd, &except_set)) {
                event |= Event::error;
            }
            if (event != Event::none) {
                fired.push_back(FiredEvent{fd, event});
            }
        }
        return fired;
    }
#endif

#if defined(CPP20_SERVER_USE_EPOLL)
    int epoll_fd_{-1};
    std::vector<epoll_event> events_;
#elif defined(CPP20_SERVER_USE_KQUEUE)
    int kqueue_fd_{-1};
    std::vector<struct kevent> events_;
#else
    std::size_t max_events_{4096};
    std::unordered_map<socket_t, Event> interests_;
#endif
};

Poller::Poller(std::size_t max_events) : impl_(std::make_unique<Impl>(max_events)) {}

Poller::~Poller() = default;

Poller::Poller(Poller&&) noexcept = default;

Poller& Poller::operator=(Poller&&) noexcept = default;

void Poller::add(socket_t fd, Event events) {
    impl_->add(fd, events);
}

void Poller::modify(socket_t fd, Event events) {
    impl_->modify(fd, events);
}

void Poller::remove(socket_t fd) {
    impl_->remove(fd);
}

std::vector<FiredEvent> Poller::wait(std::chrono::milliseconds timeout) {
    return impl_->wait(timeout);
}

const char* Poller::backend_name() const noexcept {
    return impl_->backend_name();
}

} // namespace cpp20_server::net
