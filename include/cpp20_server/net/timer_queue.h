#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace cpp20_server::net {

// TimerQueue is a small min-heap based timer queue.
// Beginner explanation:
// - Every timer has an expire time.
// - The earliest timer is kept at the heap top.
// - EventLoop asks TimerQueue how long it may sleep before the next timer.
class TimerQueue {
public:
    using Clock = std::chrono::steady_clock;
    using Callback = std::function<void()>;
    using TimerId = std::uint64_t;

    TimerId run_after(std::chrono::milliseconds delay, Callback callback);
    TimerId run_every(std::chrono::milliseconds interval, Callback callback);
    void cancel(TimerId id);

    [[nodiscard]] std::chrono::milliseconds next_timeout(std::chrono::milliseconds fallback) const;
    void run_due_timers();

private:
    struct Timer {
        Clock::time_point expires_at{};
        std::chrono::milliseconds interval{0};
        std::uint64_t sequence{0};
        TimerId id{0};
        Callback callback;
        bool repeat{false};
    };

    static bool later(const Timer& lhs, const Timer& rhs) noexcept;
    void push_timer(Timer timer);

    mutable std::mutex mutex_;
    std::vector<Timer> timers_;
    std::uint64_t next_sequence_{0};
    TimerId next_timer_id_{1};
    std::unordered_set<TimerId> active_;
    std::unordered_set<TimerId> cancelled_;
};

} // namespace cpp20_server::net
