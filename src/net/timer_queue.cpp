#include "cpp20_server/net/timer_queue.h"

#include <algorithm>

namespace cpp20_server::net {

TimerQueue::TimerId TimerQueue::run_after(std::chrono::milliseconds delay, Callback callback) {
    if (!callback) {
        return 0;
    }

    if (delay < std::chrono::milliseconds{0}) {
        delay = std::chrono::milliseconds{0};
    }

    TimerId id = 0;
    {
        std::scoped_lock lock(mutex_);
        id = next_timer_id_++;
        active_.insert(id);
        push_timer(Timer{
            Clock::now() + delay,
            std::chrono::milliseconds{0},
            next_sequence_++,
            id,
            std::move(callback),
            false,
        });
    }
    return id;
}

TimerQueue::TimerId TimerQueue::run_every(std::chrono::milliseconds interval, Callback callback) {
    if (!callback) {
        return 0;
    }

    if (interval <= std::chrono::milliseconds{0}) {
        interval = std::chrono::milliseconds{1};
    }

    TimerId id = 0;
    {
        std::scoped_lock lock(mutex_);
        id = next_timer_id_++;
        active_.insert(id);
        push_timer(Timer{
            Clock::now() + interval,
            interval,
            next_sequence_++,
            id,
            std::move(callback),
            true,
        });
    }
    return id;
}

void TimerQueue::cancel(TimerId id) {
    if (id == 0) {
        return;
    }
    std::scoped_lock lock(mutex_);
    if (active_.find(id) != active_.end()) {
        cancelled_.insert(id);
    }
}

std::chrono::milliseconds TimerQueue::next_timeout(std::chrono::milliseconds fallback) const {
    std::scoped_lock lock(mutex_);
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

    {
        std::scoped_lock lock(mutex_);
        while (!timers_.empty() && timers_.front().expires_at <= now) {
            std::pop_heap(timers_.begin(), timers_.end(), later);
            due.push_back(std::move(timers_.back()));
            timers_.pop_back();
        }
    }

    for (auto& timer : due) {
        {
            std::scoped_lock lock(mutex_);
            if (cancelled_.erase(timer.id) > 0) {
                active_.erase(timer.id);
                continue;
            }
        }

        if (timer.callback) {
            timer.callback();
        }

        {
            std::scoped_lock lock(mutex_);
            if (timer.repeat && cancelled_.find(timer.id) == cancelled_.end()) {
                timer.expires_at = Clock::now() + timer.interval;
                timer.sequence = next_sequence_++;
                push_timer(std::move(timer));
            } else {
                cancelled_.erase(timer.id);
                active_.erase(timer.id);
            }
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
