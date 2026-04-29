#include "cpp20_server/net/event_loop.h"

#include "cpp20_server/net/channel.h"

#include <chrono>

namespace cpp20_server::net {

EventLoop::EventLoop(std::size_t max_events) : poller_(max_events) {}

EventLoop::~EventLoop() = default;

void EventLoop::loop() {
    while (!stopping_.load()) {
        auto fired_events = poller_.wait(std::chrono::milliseconds{1000});
        for (const auto& fired : fired_events) {
            auto it = channels_.find(fired.fd);
            if (it == channels_.end() || it->second == nullptr) {
                continue;
            }
            it->second->handle_event(fired.events);
        }
    }
}

void EventLoop::stop() noexcept {
    stopping_.store(true);
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

} // namespace cpp20_server::net
