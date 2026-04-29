#include "cpp20_server/base/logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace cpp20_server::base {

namespace {

std::string lower_copy(std::string_view value) {
    std::string result{value};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::tm local_time(std::time_t value) {
    std::tm result{};
#if defined(_WIN32)
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

std::string format_log_line(LogLevel level, std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto tm = local_time(time);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count()
        << " [" << log_level_name(level) << "] "
        << message;
    return out.str();
}

} // namespace

std::string_view log_level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace:
        return "TRACE";
    case LogLevel::debug:
        return "DEBUG";
    case LogLevel::info:
        return "INFO";
    case LogLevel::warn:
        return "WARN";
    case LogLevel::error:
        return "ERROR";
    }
    return "INFO";
}

LogLevel parse_log_level(std::string_view value) {
    const auto lower = lower_copy(value);
    if (lower == "trace") {
        return LogLevel::trace;
    }
    if (lower == "debug") {
        return LogLevel::debug;
    }
    if (lower == "info") {
        return LogLevel::info;
    }
    if (lower == "warn" || lower == "warning") {
        return LogLevel::warn;
    }
    if (lower == "error") {
        return LogLevel::error;
    }
    throw std::runtime_error("unknown log level: " + std::string{value});
}

AsyncLogger::AsyncLogger(LoggerOptions options)
    : options_(std::move(options)) {}

AsyncLogger::~AsyncLogger() {
    stop();
}

void AsyncLogger::start() {
    std::scoped_lock lock(mutex_);
    if (started_) {
        return;
    }
    if (!options_.file_path.empty()) {
        file_.open(options_.file_path, std::ios::app);
        if (!file_) {
            throw std::runtime_error("failed to open log file: " + options_.file_path);
        }
    }
    stopping_ = false;
    started_ = true;
    worker_ = std::thread([this] { worker_loop(); });
}

void AsyncLogger::stop() noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (!started_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    std::scoped_lock lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    started_ = false;
}

void AsyncLogger::log(LogLevel level, std::string_view message) {
    if (!should_log(level)) {
        return;
    }

    start();
    {
        std::scoped_lock lock(mutex_);
        if (queue_.size() >= options_.max_queue_size && !queue_.empty()) {
            queue_.pop_front();
        }
        queue_.push_back(format_log_line(level, message));
    }
    cv_.notify_one();
}

void AsyncLogger::trace(std::string_view message) {
    log(LogLevel::trace, message);
}

void AsyncLogger::debug(std::string_view message) {
    log(LogLevel::debug, message);
}

void AsyncLogger::info(std::string_view message) {
    log(LogLevel::info, message);
}

void AsyncLogger::warn(std::string_view message) {
    log(LogLevel::warn, message);
}

void AsyncLogger::error(std::string_view message) {
    log(LogLevel::error, message);
}

bool AsyncLogger::should_log(LogLevel level) const noexcept {
    return static_cast<int>(level) >= static_cast<int>(options_.level);
}

void AsyncLogger::worker_loop() {
    for (;;) {
        std::deque<std::string> batch;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });
            if (queue_.empty() && stopping_) {
                return;
            }
            batch.swap(queue_);
        }

        for (const auto& line : batch) {
            if (options_.console) {
                std::cout << line << '\n';
            }
            if (file_.is_open()) {
                file_ << line << '\n';
            }
        }
        if (file_.is_open()) {
            file_.flush();
        }
    }
}

} // namespace cpp20_server::base
