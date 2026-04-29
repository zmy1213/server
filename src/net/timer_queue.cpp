#include "cpp20_server/net/timer_queue.h"

#include <algorithm>

namespace cpp20_server::net {

void TimerQueue::run_after(std::chrono::milliseconds delay, Callback callback) {
    if (!callback) {
        return;
    }

    if (delay < std::chrono::milliseconds{0}) {
        delay = std::chrono::milliseconds{0};
    }

    push_timer(Timer{
        Clock::now() + delay,
        std::chrono::milliseconds{0},
        next_sequence_++,
        std::move(callback),
        false,
    });
}

void TimerQueue::run_every(std::chrono::milliseconds interval, Callback callback) {
    if (!callback) {
        return;
    }

    if (interval <= std::chrono::milliseconds{0}) {
        interval = std::chrono::milliseconds{1};
    }

    push_timer(Timer{
        Clock::now() + interval,
        interval,
        next_sequence_++,
        std::move(callback),
        true,
    });
}

std::chrono::milliseconds TimerQueue::next_timeout(std::chrono::milliseconds fallback) const {
    if (timers_.empty()) {
        return fallback;
    }

    const auto now = Clock::now();
    const auto next_expire = timers_.front().expires_at;
    if (next_expire <= now) {
        return std::chrono::milliseconds{0};
    }

    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_expire - now);
    return std::min(wait, fallback);
}

void TimerQueue::run_due_timers() {
    const auto now = Clock::now();
    std::vector<Timer> due;

    while (!timers_.empty() && timers_.front().expires_at <= now) {
        std::pop_heap(timers_.begin(), timers_.end(), later);
        due.push_back(std::move(timers_.back()));
        timers_.pop_back();
    }

    for (auto& timer : due) {
        if (timer.callback) {
            timer.callback();
        }
        if (timer.repeat) {
            timer.expires_at = Clock::now() + timer.interval;
            timer.sequence = next_sequence_++;
            push_timer(std::move(timer));
        }
    }
}

bool TimerQueue::later(const Timer& lhs, const Timer& rhs) noexcept {
    if (lhs.expires_at == rhs.expires_at) {
        return lhs.sequence > rhs.sequence;
    }
    return lhs.expires_at > rhs.expires_at;
}

void TimerQueue::push_timer(Timer timer) {
    timers_.push_back(std::move(timer));
    std::push_heap(timers_.begin(), timers_.end(), later);
}

} // namespace cpp20_server::net
