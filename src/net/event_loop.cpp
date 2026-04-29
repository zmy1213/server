#include "cpp20_server/net/event_loop.h"

#include "cpp20_server/net/channel.h"

#include <chrono>

namespace cpp20_server::net {

EventLoop::EventLoop(std::size_t max_events) : poller_(max_events) {}

EventLoop::~EventLoop() = default;

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
}

void EventLoop::run_in_loop(Task task) {
    std::scoped_lock lock(pending_mutex_);
    pending_tasks_.push_back(std::move(task));
}

void EventLoop::run_after(std::chrono::milliseconds delay, Task task) {
    run_in_loop([this, delay, task = std::move(task)]() mutable {
        timer_queue_.run_after(delay, std::move(task));
    });
}

void EventLoop::run_every(std::chrono::milliseconds interval, Task task) {
    run_in_loop([this, interval, task = std::move(task)]() mutable {
        timer_queue_.run_every(interval, std::move(task));
    });
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
