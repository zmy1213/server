#pragma once

#include "cpp20_server/net/poller.h"
#include "cpp20_server/net/socket.h"

#include <functional>

namespace cpp20_server::net {

class EventLoop;

// Channel represents "one fd and the events it cares about".
// It is the small adapter between the OS event result and C++ callbacks.
class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop& loop, socket_t fd);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    [[nodiscard]] socket_t fd() const noexcept;
    [[nodiscard]] Event events() const noexcept;
    [[nodiscard]] bool is_registered() const noexcept;

    void set_read_callback(EventCallback callback);
    void set_write_callback(EventCallback callback);
    void set_close_callback(EventCallback callback);
    void set_error_callback(EventCallback callback);

    void enable_reading();
    void enable_writing();
    void disable_writing();
    void disable_all();
    void remove();

    void mark_registered(bool value) noexcept;
    void handle_event(Event fired_events);

private:
    void rebuild_events();
    void update();

    EventLoop& loop_;
    socket_t fd_{invalid_socket};
    bool registered_{false};
    bool reading_{false};
    bool writing_{false};
    Event events_{Event::none};
    EventCallback read_callback_;
    EventCallback write_callback_;
    EventCallback close_callback_;
    EventCallback error_callback_;
};

} // namespace cpp20_server::net
