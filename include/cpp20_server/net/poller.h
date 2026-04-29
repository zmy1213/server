#pragma once

#include "cpp20_server/net/socket.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace cpp20_server::net {

enum class Event : std::uint32_t {
    none = 0,
    read = 1U << 0U,
    write = 1U << 1U,
    error = 1U << 2U,
    close = 1U << 3U,
};

constexpr Event operator|(Event lhs, Event rhs) noexcept {
    return static_cast<Event>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr Event operator&(Event lhs, Event rhs) noexcept {
    return static_cast<Event>(
        static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

constexpr Event& operator|=(Event& lhs, Event rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool has_event(Event value, Event flag) noexcept {
    return static_cast<std::uint32_t>(value & flag) != 0U;
}

struct FiredEvent {
    socket_t fd{invalid_socket};
    Event events{Event::none};
};

class Poller {
public:
    explicit Poller(std::size_t max_events = 4096);
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    Poller(Poller&&) noexcept;
    Poller& operator=(Poller&&) noexcept;

    void add(socket_t fd, Event events);
    void modify(socket_t fd, Event events);
    void remove(socket_t fd);

    [[nodiscard]] std::vector<FiredEvent> wait(std::chrono::milliseconds timeout);
    [[nodiscard]] const char* backend_name() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cpp20_server::net
