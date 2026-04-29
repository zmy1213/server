#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace cpp20_server::base {

enum class LogLevel {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
};

struct LoggerOptions {
    LogLevel level{LogLevel::info};
    std::string file_path{};
    bool console{true};
    std::size_t max_queue_size{8192};
};

[[nodiscard]] std::string_view log_level_name(LogLevel level) noexcept;
[[nodiscard]] LogLevel parse_log_level(std::string_view value);

class AsyncLogger {
public:
    explicit AsyncLogger(LoggerOptions options = {});
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void start();
    void stop() noexcept;
    void log(LogLevel level, std::string_view message);

    void trace(std::string_view message);
    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);

private:
    [[nodiscard]] bool should_log(LogLevel level) const noexcept;
    void worker_loop();

    LoggerOptions options_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::ofstream file_;
    std::thread worker_;
    bool started_{false};
    bool stopping_{false};
};

} // namespace cpp20_server::base
