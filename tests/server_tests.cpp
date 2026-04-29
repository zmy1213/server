#include "cpp20_server/net/buffer.h"
#include "cpp20_server/net/socket.h"
#include "cpp20_server/net/tcp_server.h"
#include "cpp20_server/net/timer_queue.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
using cpp20_server::net::SocketRuntime;
using cpp20_server::net::TcpServer;
using cpp20_server::net::TcpServerOptions;
using cpp20_server::net::TimerQueue;
using cpp20_server::net::close_socket;
using cpp20_server::net::invalid_socket;
using cpp20_server::net::is_would_block;
using cpp20_server::net::last_socket_error;
using cpp20_server::net::socket_t;

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

class RunningServer {
public:
    RunningServer(std::uint16_t port, std::size_t worker_threads, std::uint64_t idle_timeout_seconds = 0)
        : port_(port) {
        TcpServerOptions options;
        options.host = "127.0.0.1";
        options.port = port;
        options.worker_threads = worker_threads;
        options.idle_timeout_seconds = idle_timeout_seconds;
        server_ = std::make_unique<TcpServer>(std::move(options));
        server_->set_message_callback([](std::string_view message) {
            return std::string{message};
        });

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

void test_single_connection_echo() {
    // 集成测试：启动一个真实 TcpServer，客户端发一条消息，要求原样返回。
    const std::uint16_t port = find_free_loopback_port();
    RunningServer server{port, 2};

    const std::string message = "single-echo-test\n";
    const std::string response = echo_once(port, message);
    expect(response == message, "single connection echo response mismatch");
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
        run_test("single_connection_echo", test_single_connection_echo);
        run_test("concurrent_echo", test_concurrent_echo);
        run_test("idle_timeout", test_idle_timeout_closes_inactive_connection);
        std::cout << "All tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << '\n';
        return 1;
    }
}
