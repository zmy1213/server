#include "cpp20_server/base/config.h"
#include "cpp20_server/base/logger.h"
#include "cpp20_server/base/metrics.h"
#include "cpp20_server/net/buffer.h"
#include "cpp20_server/net/socket.h"
#include "cpp20_server/net/tcp_server.h"
#include "cpp20_server/net/timer_queue.h"
#include "cpp20_server/protocol/http.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using cpp20_server::net::Buffer;
using cpp20_server::base::AsyncLogger;
using cpp20_server::base::Config;
using cpp20_server::base::LogLevel;
using cpp20_server::base::LoggerOptions;
using cpp20_server::base::format_server_stats_prometheus;
using cpp20_server::base::format_server_stats_text;
using cpp20_server::net::SocketRuntime;
using cpp20_server::net::ServerStats;
using cpp20_server::net::TcpServer;
using cpp20_server::net::TcpServerOptions;
using cpp20_server::net::TimerQueue;
using cpp20_server::net::close_socket;
using cpp20_server::net::invalid_socket;
using cpp20_server::net::is_would_block;
using cpp20_server::net::last_socket_error;
using cpp20_server::net::socket_t;
using cpp20_server::protocol::handle_demo_http_request;
using cpp20_server::protocol::handle_demo_http_stream;
using cpp20_server::protocol::make_http_response;
using cpp20_server::protocol::parse_http_request;
using cpp20_server::protocol::try_parse_http_request;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

void set_receive_timeout(socket_t fd, int milliseconds) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

std::uint16_t find_free_loopback_port() {
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == invalid_socket) {
        throw std::system_error(last_socket_error(), "socket failed while finding free port");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        auto error = last_socket_error();
        close_socket(fd);
        throw std::system_error(error, "bind failed while finding free port");
    }

#if defined(_WIN32)
    int length = sizeof(addr);
#else
    socklen_t length = sizeof(addr);
#endif
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
        auto error = last_socket_error();
        close_socket(fd);
        throw std::system_error(error, "getsockname failed while finding free port");
    }

    const auto port = static_cast<std::uint16_t>(ntohs(addr.sin_port));
    close_socket(fd);
    return port;
}

socket_t connect_to(std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* raw_result = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = getaddrinfo("127.0.0.1", port_text.c_str(), &hints, &raw_result);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo failed in client");
    }

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> result{raw_result, freeaddrinfo};
    std::error_code last_error;
    for (addrinfo* current = result.get(); current != nullptr; current = current->ai_next) {
        socket_t fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd == invalid_socket) {
            last_error = last_socket_error();
            continue;
        }

        if (::connect(fd, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
            set_receive_timeout(fd, 3000);
            return fd;
        }

        last_error = last_socket_error();
        close_socket(fd);
    }

    throw std::system_error(last_error, "client connect failed");
}

bool try_connect_once(std::uint16_t port) {
    try {
        socket_t fd = connect_to(port);
        close_socket(fd);
        return true;
    } catch (...) {
        return false;
    }
}

void send_all(socket_t fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto remaining = data.size() - sent;
        const int chunk = remaining > static_cast<std::size_t>(INT_MAX)
                              ? INT_MAX
                              : static_cast<int>(remaining);
        const auto n = ::send(fd, data.data() + sent, chunk, 0);
        if (n <= 0) {
            throw std::system_error(last_socket_error(), "client send failed");
        }
        sent += static_cast<std::size_t>(n);
    }
}

void shutdown_write(socket_t fd) {
#if defined(_WIN32)
    shutdown(fd, SD_SEND);
#else
    shutdown(fd, SHUT_WR);
#endif
}

std::string recv_exact_or_until_close(socket_t fd, std::size_t expected_bytes) {
    std::string result;
    result.reserve(expected_bytes);
    while (result.size() < expected_bytes) {
        char buffer[4096]{};
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            result.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        throw std::system_error(last_socket_error(), "client recv failed");
    }
    return result;
}

std::string recv_until_close(socket_t fd) {
    std::string result;
    for (;;) {
        char buffer[4096]{};
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            result.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            return result;
        }
        throw std::system_error(last_socket_error(), "client recv failed");
    }
}

std::string echo_once(std::uint16_t port, std::string_view message) {
    socket_t fd = connect_to(port);
    try {
        send_all(fd, message);
        shutdown_write(fd);
        std::string response = recv_exact_or_until_close(fd, message.size());
        close_socket(fd);
        return response;
    } catch (...) {
        close_socket(fd);
        throw;
    }
}

std::string request_once(std::uint16_t port, std::string_view request) {
    socket_t fd = connect_to(port);
    try {
        send_all(fd, request);
        shutdown_write(fd);
        std::string response = recv_until_close(fd);
        close_socket(fd);
        return response;
    } catch (...) {
        close_socket(fd);
        throw;
    }
}

bool wait_until_remote_close(socket_t fd) {
    char buffer[1]{};
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
        return true;
    }
    if (n < 0) {
        return !is_would_block(last_socket_error());
    }
    return false;
}

std::size_t http_body_size(std::string_view response) {
    const auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        throw std::runtime_error("http response header is incomplete");
    }

    constexpr std::string_view header_name = "Content-Length:";
    const auto content_length = response.find(header_name);
    if (content_length == std::string_view::npos || content_length > header_end) {
        throw std::runtime_error("http response is missing content-length");
    }

    auto value = response.substr(content_length + header_name.size());
    const auto line_end = value.find("\r\n");
    value = value.substr(0, line_end);
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }

    return static_cast<std::size_t>(std::stoul(std::string{value}));
}

std::string recv_one_http_response(socket_t fd, std::string& inbox) {
    for (;;) {
        const auto header_end = inbox.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const auto body_size = http_body_size(inbox);
            const auto total_size = header_end + 4 + body_size;
            if (inbox.size() >= total_size) {
                std::string response = inbox.substr(0, total_size);
                inbox.erase(0, total_size);
                return response;
            }
        }

        char buffer[4096]{};
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            inbox.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            throw std::runtime_error("server closed before a complete http response arrived");
        }
        throw std::system_error(last_socket_error(), "client recv failed");
    }
}

class RunningServer {
public:
    RunningServer(std::uint16_t port,
                  std::size_t worker_threads,
                  std::uint64_t idle_timeout_seconds = 0,
                  TcpServer::MessageCallback callback = {})
        : port_(port) {
        TcpServerOptions options;
        options.host = "127.0.0.1";
        options.port = port;
        options.worker_threads = worker_threads;
        options.idle_timeout_seconds = idle_timeout_seconds;
        server_ = std::make_unique<TcpServer>(std::move(options));
        if (callback) {
            server_->set_message_callback(std::move(callback));
        } else {
            server_->set_message_callback([](std::string_view message) {
                return std::string{message};
            });
        }

        thread_ = std::thread([this] {
            try {
                server_->start();
            } catch (...) {
                server_error_ = std::current_exception();
            }
        });

        wait_until_ready();
    }

    RunningServer(std::uint16_t port,
                  std::size_t worker_threads,
                  std::uint64_t idle_timeout_seconds,
                  TcpServer::StreamCallback callback)
        : port_(port) {
        TcpServerOptions options;
        options.host = "127.0.0.1";
        options.port = port;
        options.worker_threads = worker_threads;
        options.idle_timeout_seconds = idle_timeout_seconds;
        server_ = std::make_unique<TcpServer>(std::move(options));
        server_->set_stream_callback(std::move(callback));

        thread_ = std::thread([this] {
            try {
                server_->start();
            } catch (...) {
                server_error_ = std::current_exception();
            }
        });

        wait_until_ready();
    }

    ~RunningServer() {
        stop();
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    void stop() {
        if (server_) {
            server_->stop();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (server_error_) {
            std::rethrow_exception(server_error_);
        }
    }

private:
    void wait_until_ready() {
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (server_error_) {
                std::rethrow_exception(server_error_);
            }
            if (try_connect_once(port_)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        throw std::runtime_error("server did not start listening in time");
    }

    std::uint16_t port_{0};
    std::unique_ptr<TcpServer> server_;
    std::thread thread_;
    std::exception_ptr server_error_;
};

void test_timer_queue() {
    TimerQueue timers;
    int one_shot_count = 0;
    int repeat_count = 0;

    timers.run_after(std::chrono::milliseconds{0}, [&one_shot_count] {
        ++one_shot_count;
    });
    timers.run_due_timers();
    expect(one_shot_count == 1, "one-shot timer should fire once");

    timers.run_every(std::chrono::milliseconds{1}, [&repeat_count] {
        ++repeat_count;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    timers.run_due_timers();
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    timers.run_due_timers();
    expect(repeat_count >= 2, "repeat timer should fire more than once");
}

void test_buffer() {
    // 单元测试：只验证 Buffer 自己的读写位置和内容，不启动服务器。
    Buffer buffer;
    expect(buffer.empty(), "new buffer should be empty");

    buffer.append("hello");
    expect(!buffer.empty(), "buffer should not be empty after append");
    expect(buffer.readable_bytes() == 5, "buffer readable size should be 5");
    expect(buffer.readable_view() == "hello", "buffer content should be hello");

    buffer.retrieve(2);
    expect(buffer.readable_view() == "llo", "buffer retrieve should advance read position");

    buffer.append(" world");
    expect(buffer.readable_view() == "llo world", "buffer append should preserve readable suffix");

    buffer.retrieve(999);
    expect(buffer.empty(), "over-retrieve should clear buffer");
}

void test_config_parser() {
    const auto config = Config::from_text(
        "# comments are ignored\n"
        "host = 127.0.0.1\n"
        "port = 9090\n"
        "worker_threads = 4\n"
        "idle_timeout_seconds = 30\n"
        "log_console = false\n");

    expect(config.get_string("host") == "127.0.0.1", "config host should be parsed");
    expect(config.get_port("port", 8080) == 9090, "config port should be parsed");
    expect(config.get_size("worker_threads", 0) == 4, "config worker_threads should be parsed");
    expect(config.get_u64("idle_timeout_seconds", 0) == 30, "config timeout should be parsed");
    expect(!config.get_bool("log_console", true), "config bool should be parsed");
    expect(config.get_string("missing", "fallback") == "fallback", "config default should be returned");
}

void test_async_logger() {
    const auto log_path = std::filesystem::temp_directory_path()
                          / ("cpp20_server_test_"
                             + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                             + ".log");

    LoggerOptions options;
    options.level = LogLevel::debug;
    options.file_path = log_path.string();
    options.console = false;

    {
        AsyncLogger logger{options};
        logger.debug("debug line");
        logger.info("info line");
        logger.stop();
    }

    std::ifstream file{log_path};
    std::string content{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    std::filesystem::remove(log_path);

    expect(content.find("[DEBUG] debug line") != std::string::npos,
           "async logger should write debug line");
    expect(content.find("[INFO] info line") != std::string::npos,
           "async logger should write info line");
}

void test_metrics_formatter() {
    ServerStats stats;
    stats.accepted_connections = 10;
    stats.active_connections = 2;
    stats.bytes_read = 128;
    stats.bytes_written = 256;

    const auto text = format_server_stats_text(stats);
    expect(text.find("accepted_connections=10") != std::string::npos,
           "text metrics should include accepted connections");
    expect(text.find("bytes_written=256") != std::string::npos,
           "text metrics should include written bytes");

    const auto prometheus = format_server_stats_prometheus(stats, "kqueue");
    expect(prometheus.find("cpp20_server_accepted_connections_total{backend=\"kqueue\"} 10")
               != std::string::npos,
           "prometheus metrics should include backend label");
    expect(prometheus.find("cpp20_server_active_connections{backend=\"kqueue\"} 2")
               != std::string::npos,
           "prometheus metrics should include active gauge");
}

void test_http_parser() {
    const std::string request =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "hello http";
    const auto parsed = parse_http_request(request);
    expect(parsed.has_value(), "http parser should parse complete request");
    expect(parsed->method == "POST", "http method should be POST");
    expect(parsed->path == "/echo", "http path should be /echo");
    expect(parsed->version == "HTTP/1.1", "http version should be HTTP/1.1");
    expect(parsed->headers.at("host") == "localhost", "http host header should be parsed");
    expect(parsed->body == "hello http", "http body should match content-length");

    const auto parsed_with_size = try_parse_http_request(request + "GET /health HTTP/1.1\r\n\r\n");
    expect(parsed_with_size.has_value(), "http parser should report consumed bytes");
    expect(parsed_with_size->bytes_consumed == request.size(),
           "http parser consumed size should stop after first request");

    const auto incomplete = parse_http_request("GET /health HTTP/1.1\r\nHost: localhost\r\n");
    expect(!incomplete.has_value(), "http parser should wait for complete header");
}

void test_http_stream_handles_partial_and_sticky_requests() {
    Buffer input;
    Buffer output;

    input.append("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nhello");
    handle_demo_http_stream(input, output);
    expect(output.empty(), "partial http request should not produce a response yet");
    expect(!input.empty(), "partial http request should stay in input buffer");

    input.append(" worldGET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    handle_demo_http_stream(input, output);
    const auto responses = output.readable_view();
    expect(responses.find("hello world") != std::string_view::npos,
           "completed partial request should echo body");
    expect(responses.find("ok\n") != std::string_view::npos,
           "sticky second request should also be handled");
    expect(input.empty(), "all complete sticky requests should be consumed");
}

void test_http_response_routes() {
    const std::string health_response = handle_demo_http_request(
        "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(health_response.find("HTTP/1.1 200 OK\r\n") == 0, "GET /health should return 200");
    expect(health_response.find("ok\n") != std::string::npos, "GET /health body should be ok");

    const std::string echo_response = handle_demo_http_request(
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello");
    expect(echo_response.find("HTTP/1.1 200 OK\r\n") == 0, "POST /echo should return 200");
    expect(echo_response.find("\r\n\r\nhello") != std::string::npos, "POST /echo should echo body");

    const std::string missing_response = handle_demo_http_request(
        "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(missing_response.find("HTTP/1.1 404 Not Found\r\n") == 0, "unknown route should return 404");

    const std::string raw_response = make_http_response(201, "Created", "created\n");
    expect(raw_response.find("Content-Length: 8\r\n") != std::string::npos,
           "http response should include content length");
}

void test_single_connection_echo() {
    // 集成测试：启动一个真实 TcpServer，客户端发一条消息，要求原样返回。
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 2};

    const std::string message = "single-echo-test\n";
    const std::string response = echo_once(port, message);
    expect(response == message, "single connection echo response mismatch");
}

void test_http_server_health() {
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 2, 0, TcpServer::StreamCallback{[](Buffer& input, Buffer& output) {
        handle_demo_http_stream(input, output);
    }}};

    const std::string response = request_once(
        port,
        "GET /health HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n");
    expect(response.find("HTTP/1.1 200 OK\r\n") == 0, "http server GET /health should return 200");
    expect(response.find("\r\n\r\nok\n") != std::string::npos, "http server GET /health body should be ok");
}

void test_http_server_post_echo() {
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 2, 0, TcpServer::StreamCallback{[](Buffer& input, Buffer& output) {
        handle_demo_http_stream(input, output);
    }}};

    const std::string response = request_once(
        port,
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "hello server");
    expect(response.find("HTTP/1.1 200 OK\r\n") == 0, "http server POST /echo should return 200");
    expect(response.find("\r\n\r\nhello server") != std::string::npos,
           "http server POST /echo should echo request body");
}

void test_http_server_partial_request() {
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 2, 0, TcpServer::StreamCallback{[](Buffer& input, Buffer& output) {
        handle_demo_http_stream(input, output);
    }}};

    socket_t fd = connect_to(port);
    try {
        send_all(fd, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nhello");
        std::this_thread::sleep_for(std::chrono::milliseconds{80});
        send_all(fd, " world");
        shutdown_write(fd);
        const std::string response = recv_until_close(fd);
        close_socket(fd);
        expect(response.find("HTTP/1.1 200 OK\r\n") == 0,
               "partial http request should eventually return 200");
        expect(response.find("\r\n\r\nhello world") != std::string::npos,
               "partial http request should keep first half in input buffer");
    } catch (...) {
        close_socket(fd);
        throw;
    }
}

void test_http_server_sticky_requests() {
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 2, 0, TcpServer::StreamCallback{[](Buffer& input, Buffer& output) {
        handle_demo_http_stream(input, output);
    }}};

    const std::string response = request_once(
        port,
        "GET /health HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n"
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello");
    const auto first = response.find("HTTP/1.1 200 OK\r\n");
    const auto second = response.find("HTTP/1.1 200 OK\r\n", first + 1);
    expect(first != std::string::npos, "sticky requests should produce first response");
    expect(second != std::string::npos, "sticky requests should produce second response");
    expect(response.find("\r\n\r\nok\n") != std::string::npos,
           "sticky first response should be health body");
    expect(response.find("\r\n\r\nhello") != std::string::npos,
           "sticky second response should be echo body");
}

void test_http_server_multiple_requests_same_connection() {
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 2, 0, TcpServer::StreamCallback{[](Buffer& input, Buffer& output) {
        handle_demo_http_stream(input, output);
    }}};

    socket_t fd = connect_to(port);
    std::string inbox;
    try {
        send_all(fd, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        const std::string first = recv_one_http_response(fd, inbox);
        expect(first.find("\r\n\r\nok\n") != std::string::npos,
               "first keep-alive request should return health body");

        send_all(fd,
                 "POST /echo HTTP/1.1\r\n"
                 "Host: localhost\r\n"
                 "Content-Length: 6\r\n"
                 "\r\n"
                 "second");
        shutdown_write(fd);
        const std::string second = recv_one_http_response(fd, inbox);
        close_socket(fd);
        expect(second.find("\r\n\r\nsecond") != std::string::npos,
               "second request on same connection should return echo body");
    } catch (...) {
        close_socket(fd);
        throw;
    }
}

void test_concurrent_echo() {
    // 并发测试：多个客户端同时连接同一个服务器，验证多线程 Reactor 能正常回显。
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 4};

    constexpr int client_count = 64;
    std::atomic_int failures{0};
    std::mutex errors_mutex;
    std::vector<std::string> errors;
    std::vector<std::thread> clients;
    clients.reserve(client_count);

    for (int i = 0; i < client_count; ++i) {
        clients.emplace_back([port, i, &failures, &errors, &errors_mutex] {
            try {
                const std::string message = "concurrent-client-" + std::to_string(i) + "\n";
                const std::string response = echo_once(port, message);
                if (response != message) {
                    ++failures;
                    std::scoped_lock lock(errors_mutex);
                    errors.push_back("response mismatch for client " + std::to_string(i));
                }
            } catch (const std::exception& ex) {
                ++failures;
                std::scoped_lock lock(errors_mutex);
                errors.push_back(ex.what());
            }
        });
    }

    for (auto& client : clients) {
        client.join();
    }

    if (!errors.empty()) {
        std::cerr << "first concurrent echo error: " << errors.front() << '\n';
    }
    expect(failures.load() == 0, "concurrent echo test had failures");
}

void test_idle_timeout_closes_inactive_connection() {
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 2, 1};

    socket_t fd = connect_to(port);
    try {
        const bool closed = wait_until_remote_close(fd);
        close_socket(fd);
        expect(closed, "idle connection should be closed by server");
    } catch (...) {
        close_socket(fd);
        throw;
    }
}

void run_test(std::string_view name, void (*test)()) {
    const auto start = std::chrono::steady_clock::now();
    test();
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "[PASS] " << name << " (" << elapsed << " ms)\n";
}

} // namespace

int main() {
    try {
        SocketRuntime runtime;
        run_test("timer_queue", test_timer_queue);
        run_test("buffer", test_buffer);
        run_test("config_parser", test_config_parser);
        run_test("async_logger", test_async_logger);
        run_test("metrics_formatter", test_metrics_formatter);
        run_test("http_parser", test_http_parser);
        run_test("http_stream", test_http_stream_handles_partial_and_sticky_requests);
        run_test("http_response_routes", test_http_response_routes);
        run_test("single_connection_echo", test_single_connection_echo);
        run_test("http_server_health", test_http_server_health);
        run_test("http_server_post_echo", test_http_server_post_echo);
        run_test("http_server_partial_request", test_http_server_partial_request);
        run_test("http_server_sticky_requests", test_http_server_sticky_requests);
        run_test("http_server_same_connection", test_http_server_multiple_requests_same_connection);
        run_test("concurrent_echo", test_concurrent_echo);
        run_test("idle_timeout", test_idle_timeout_closes_inactive_connection);
        std::cout << "All tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << '\n';
        return 1;
    }
}
