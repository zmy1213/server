#include "cpp20_server/net/channel.h"

#include "cpp20_server/net/event_loop.h"

namespace cpp20_server::net {

Channel::Channel(EventLoop& loop, socket_t fd) : loop_(loop), fd_(fd) {}

Channel::~Channel() = default;

socket_t Channel::fd() const noexcept {
    return fd_;
}

Event Channel::events() const noexcept {
    return events_;
}

bool Channel::is_registered() const noexcept {
    return registered_;
}

void Channel::set_read_callback(EventCallback callback) {
    read_callback_ = std::move(callback);
}

void Channel::set_write_callback(EventCallback callback) {
    write_callback_ = std::move(callback);
}

void Channel::set_close_callback(EventCallback callback) {
    close_callback_ = std::move(callback);
}

void Channel::set_error_callback(EventCallback callback) {
    error_callback_ = std::move(callback);
}

void Channel::enable_reading() {
    reading_ = true;
    rebuild_events();
    update();
}

void Channel::enable_writing() {
    writing_ = true;
    rebuild_events();
    update();
}

void Channel::disable_writing() {
    writing_ = false;
    rebuild_events();
    update();
}

void Channel::disable_all() {
    reading_ = false;
    writing_ = false;
    rebuild_events();
    if (registered_) {
        remove();
    }
}

void Channel::remove() {
    if (!registered_) {
        return;
    }
    loop_.remove_channel(*this);
}

void Channel::mark_registered(bool value) noexcept {
    registered_ = value;
}

void Channel::handle_event(Event fired_events) {
    // Each callback may close the connection and destroy this Channel. Return
    // immediately after invoking a callback so we never touch a deleted object.
    if (has_event(fired_events, Event::read) && read_callback_) {
        read_callback_();
        return;
    }
    if (has_event(fired_events, Event::write) && write_callback_) {
        write_callback_();
        return;
    }
    if (has_event(fired_events, Event::close) && close_callback_) {
        close_callback_();
        return;
    }
    if (has_event(fired_events, Event::error) && error_callback_) {
        error_callback_();
        return;
    }
}

void Channel::rebuild_events() {
    Event value = Event::none;
    if (reading_) {
        value |= Event::read;
    }
    if (writing_) {
        value |= Event::write;
    }
    events_ = value;
}

void Channel::update() {
    if (events_ == Event::none) {
        if (registered_) {
            remove();
        }
        return;
    }
    loop_.update_channel(*this);
}

} // namespace cpp20_server::net
