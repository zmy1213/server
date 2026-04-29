#pragma once

#include "cpp20_server/net/poller.h"
#include "cpp20_server/net/socket.h"
#include "cpp20_server/net/timer_queue.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cpp20_server::net {

class Channel;

// EventLoop owns a Poller and dispatches fired events to Channel callbacks.
// Linux/macOS use this Reactor loop. Windows IOCP has a separate Proactor path.
class EventLoop {
public:
    using Task = std::function<void()>;

    explicit EventLoop(std::size_t max_events = 4096);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();
    void stop() noexcept;
    void run_in_loop(Task task);
    void run_after(std::chrono::milliseconds delay, Task task);
    void run_every(std::chrono::milliseconds interval, Task task);

    void update_channel(Channel& channel);
    void remove_channel(Channel& channel);

    [[nodiscard]] const char* backend_name() const noexcept;

private:
    void process_pending_tasks();

    Poller poller_;
    TimerQueue timer_queue_;
    std::unordered_map<socket_t, Channel*> channels_;
    std::mutex pending_mutex_;
    std::vector<Task> pending_tasks_;
    std::atomic_bool stopping_{false};
};

} // namespace cpp20_server::net
