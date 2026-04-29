#include "cpp20_server/base/config.h"
#include "cpp20_server/base/logger.h"
#include "cpp20_server/base/metrics.h"
#include "cpp20_server/net/tcp_server.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

cpp20_server::net::TcpServer* g_server = nullptr;

void handle_signal(int) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

std::uint16_t parse_port(const char* value) {
    const int port = std::atoi(value);
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(port);
}

std::size_t parse_worker_threads(const char* value) {
    const long count = std::strtol(value, nullptr, 10);
    if (count < 0) {
        throw std::runtime_error("worker_threads must be greater than or equal to 0");
    }
    return static_cast<std::size_t>(count);
}

std::uint64_t parse_idle_timeout_seconds(const char* value) {
    const long seconds = std::strtol(value, nullptr, 10);
    if (seconds < 0) {
        throw std::runtime_error("idle_timeout_seconds must be greater than or equal to 0");
    }
    return static_cast<std::uint64_t>(seconds);
}

cpp20_server::base::Config load_config_if_requested(int argc, char* argv[], int& arg_index) {
    cpp20_server::base::Config config;
    if (argc >= 3 && std::string_view{argv[1]} == "--config") {
        config = cpp20_server::base::Config::from_file(argv[2]);
        arg_index = 3;
    }
    return config;
}

void apply_config(const cpp20_server::base::Config& config,
                  cpp20_server::net::TcpServerOptions& options) {
    options.host = config.get_string("host", options.host);
    options.port = config.get_port("port", options.port);
    options.backlog = config.get_int("backlog", options.backlog);
    options.max_events = config.get_size("max_events", options.max_events);
    options.worker_threads = config.get_size("worker_threads", options.worker_threads);
    options.idle_timeout_seconds = config.get_u64("idle_timeout_seconds", options.idle_timeout_seconds);
}

cpp20_server::base::LoggerOptions logger_options_from_config(const cpp20_server::base::Config& config) {
    cpp20_server::base::LoggerOptions options;
    options.level = cpp20_server::base::parse_log_level(config.get_string("log_level", "info"));
    options.file_path = config.get_string("log_file");
    options.console = config.get_bool("log_console", true);
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        int arg_index = 1;
        const auto config = load_config_if_requested(argc, argv, arg_index);

        cpp20_server::net::TcpServerOptions options;
        apply_config(config, options);
        if (argc > arg_index) {
            options.host = argv[arg_index];
        }
        if (argc > arg_index + 1) {
            options.port = parse_port(argv[arg_index + 1]);
        }
        if (argc > arg_index + 2) {
            options.worker_threads = parse_worker_threads(argv[arg_index + 2]);
        }
        if (argc > arg_index + 3) {
            options.idle_timeout_seconds = parse_idle_timeout_seconds(argv[arg_index + 3]);
        }

        cpp20_server::base::AsyncLogger logger{logger_options_from_config(config)};
        logger.info("starting echo server");

        cpp20_server::net::TcpServer server{options};
        g_server = &server;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        server.set_message_callback([](std::string_view message) {
            return std::string{message};
        });

        server.start();

        const auto stats = server.stats();
        logger.info("echo server stopped: " + cpp20_server::base::format_server_stats_text(stats));
        std::cout << "stopped. " << cpp20_server::base::format_server_stats_text(stats) << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }
}
