#include "cpp20_server/net/tcp_server.h"
#include "cpp20_server/protocol/http.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
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

} // namespace

int main(int argc, char* argv[]) {
    try {
        cpp20_server::net::TcpServerOptions options;
        options.idle_timeout_seconds = 30;
        if (argc >= 2) {
            options.host = argv[1];
        }
        if (argc >= 3) {
            options.port = parse_port(argv[2]);
        }
        if (argc >= 4) {
            options.worker_threads = parse_worker_threads(argv[3]);
        }
        if (argc >= 5) {
            options.idle_timeout_seconds = parse_idle_timeout_seconds(argv[4]);
        }

        cpp20_server::net::TcpServer server{options};
        g_server = &server;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        server.set_message_callback([](std::string_view request) {
            return cpp20_server::protocol::handle_demo_http_request(request);
        });

        server.start();

        const auto& stats = server.stats();
        std::cout << "stopped. accepted=" << stats.accepted_connections
                  << " active=" << stats.active_connections
                  << " bytes_read=" << stats.bytes_read
                  << " bytes_written=" << stats.bytes_written << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }
}
