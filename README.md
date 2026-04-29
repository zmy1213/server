# C++20 Server

一个从零开始实现的 C++20 跨平台 TCP 服务器项目。

项目目标是先搭建一个清晰、可学习、可扩展的服务器骨架，再逐步升级到高并发服务器。当前版本已经支持 CMake 构建、跨平台 socket 封装、非阻塞 IO、事件循环和 Echo Server 示例。

仓库地址：

```text
https://github.com/zmy1213/server
```

文档：

```text
docs/HIGH_CONCURRENCY.md
docs/LEARNING_ROADMAP.md
docs/BENCHMARK.md
```

## 当前功能

- 使用 C++20 编写
- 使用 CMake 构建
- 支持 macOS、Linux、Windows
- 支持非阻塞 TCP socket
- 支持 Reactor 事件循环模型
- 每个连接都有输入缓冲区和输出缓冲区
- Linux 自动使用 `epoll`
- macOS 自动使用 `kqueue`
- Windows 自动使用 `IOCP`
- 提供 Echo Server 示例
- 提供 HTTP Server 示例
- 支持最小 HTTP 请求解析和响应生成
- 支持小根堆定时器和定时器取消
- 支持空闲连接超时关闭
- 支持 HTTP 路由表注册回调
- 支持简单 `key=value` 配置文件
- 支持异步日志
- 支持运行指标文本和 Prometheus 风格输出
- 支持 HTTP 压测脚本，统计 QPS、并发、延迟和错误数

说明：百万级连接不是只靠 C++ 代码就能保证，还需要系统参数、内存、端口、压测机数量一起配合。这个项目的目标是使用各系统上适合高并发的 IO 模型：Linux 用 `epoll`，macOS 用 `kqueue`，Windows 用 `IOCP`。

## 环境要求

### 通用要求

- CMake 3.20 或更高版本
- 支持 C++20 的编译器

### 推荐环境

- Linux：GCC 11+ 或 Clang 14+
- macOS：AppleClang 14+
- Windows：Visual Studio 2022 或 MinGW-w64

## 项目结构

```text
.
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── BENCHMARK.md
│   ├── HIGH_CONCURRENCY.md
│   └── LEARNING_ROADMAP.md
├── examples/
│   ├── echo_server.cpp
│   ├── http_server.cpp
│   └── server.conf
├── include/
│   └── cpp20_server/
│       ├── base/
│       │   ├── config.h
│       │   ├── logger.h
│       │   └── metrics.h
│       ├── net/
│       │   ├── acceptor.h
│       │   ├── buffer.h
│       │   ├── channel.h
│       │   ├── connection.h
│       │   ├── event_loop.h
│       │   ├── poller.h
│       │   ├── socket.h
│       │   ├── timer_queue.h
│       │   └── tcp_server.h
│       └── protocol/
│           └── http.h
├── src/
│   ├── base/
│   │   ├── config.cpp
│   │   ├── logger.cpp
│   │   └── metrics.cpp
│   ├── net/
│   │   ├── acceptor.cpp
│   │   ├── buffer.cpp
│   │   ├── channel.cpp
│   │   ├── connection.cpp
│   │   ├── event_loop.cpp
│   │   ├── poller.cpp
│   │   ├── socket.cpp
│   │   ├── timer_queue.cpp
│   │   └── tcp_server.cpp
│   └── protocol/
│       └── http.cpp
├── tests/
│   └── server_tests.cpp
└── tools/
    └── bench_http.py
```

核心模块说明：

```text
acceptor     监听 socket，专门负责 accept 新连接
buffer       输入缓冲区和输出缓冲区
config       读取 key=value 配置文件
logger       后台线程异步写日志
metrics      把 ServerStats 转成文本或 Prometheus 格式
channel      一个 fd 关心的事件和回调
connection   单个客户端连接的读写状态
event_loop   Reactor 事件循环，支持跨线程唤醒
poller       epoll/kqueue/select 的统一事件接口
socket       跨平台 socket 封装
timer_queue  小根堆定时器
tcp_server   TCP 服务器入口，Windows IOCP 也在这里实现
protocol     HTTP 请求解析和响应生成
examples     示例程序
tests        自动化功能测试
tools        压测和辅助脚本
```

## 本轮代码说明：Reactor 拆分

这一轮代码主要是在做第 4 阶段：把原来集中在 `TcpServer` 里的网络逻辑，拆成更清晰的 Reactor 组件。

简单说，这一轮不是新增复杂业务，而是在整理服务器骨架，让后面更容易继续做：

```text
多线程 Reactor
定时器
HTTP 协议
压测
百万连接优化
```

拆分前，`TcpServer` 同时负责：

```text
监听端口
accept 新连接
处理 epoll/kqueue 事件
读取客户端数据
发送响应数据
关闭连接
保存连接状态
```

这样继续往下写会越来越乱。

拆分后，职责变成：

```text
TcpServer     服务器入口，负责组装各个模块
EventLoop     事件循环，负责等待和分发事件
Channel       fd 事件封装，负责把可读/可写事件转成回调
Acceptor      监听 socket，专门负责 accept 新连接
Connection    单个客户端连接，负责 recv/send/close
Buffer        保存还没发送完的数据
Poller        封装 epoll/kqueue/select
```

现在 Linux/macOS/select 路线的代码流程是：

```text
main()
  ↓
TcpServer::start()
  ↓
创建 Acceptor
  ↓
Acceptor 监听 listen socket
  ↓
EventLoop::loop()
  ↓
Poller::wait()
  ↓
Channel::handle_event()
  ↓
如果是新连接：Acceptor::handle_read()
  ↓
创建 Connection
  ↓
如果客户端发数据：Connection::handle_read()
  ↓
业务回调生成响应
  ↓
写入 Buffer
  ↓
Connection::handle_write()
  ↓
send() 返回给客户端
```

这一轮新增的主要文件：

```text
include/cpp20_server/net/event_loop.h
src/net/event_loop.cpp

include/cpp20_server/net/channel.h
src/net/channel.cpp

include/cpp20_server/net/acceptor.h
src/net/acceptor.cpp

include/cpp20_server/net/connection.h
src/net/connection.cpp
```

这一轮修改后的意义：

```text
代码职责更清楚
TcpServer 不再什么都管
后面可以更自然地加入多个 EventLoop
每个连接可以绑定到某个 EventLoop
为多线程 Reactor 打基础
```

当前验证结果：

```text
默认 kqueue 构建通过
select 后端构建通过
warnings-as-errors 严格构建通过
单连接 echo 测试通过
64 并发 echo 测试通过
```

第 4 阶段已经完成第一版拆分；下一轮已经继续进入第 5 阶段：多线程 Reactor。

## 最新代码说明：EventLoop 唤醒机制

这一轮给 `EventLoop` 增加了自唤醒能力。

为什么需要它：

```text
EventLoop 正在 poller.wait()
        ↓
另一个线程调用 run_in_loop() 投递任务
        ↓
如果没有唤醒机制，EventLoop 可能要等 poller 超时后才执行任务
        ↓
stop() 退出也可能不够及时
```

现在的做法：

```text
EventLoop 内部创建一对 wakeup socket
        ↓
读端注册进 epoll/kqueue/select
        ↓
其他线程调用 run_in_loop() / stop() / cancel_timer()
        ↓
往写端写 1 个字节
        ↓
poller.wait() 立刻返回
        ↓
EventLoop drain_wakeup() 清空唤醒字节
        ↓
执行 pending task 或退出循环
```

小白理解：

```text
以前：EventLoop 像睡觉的人，最多 50ms 自己醒一次看有没有任务
现在：别人有任务时可以按门铃，EventLoop 立刻醒
```

核心代码：

```text
include/cpp20_server/net/event_loop.h
src/net/event_loop.cpp
tests/server_tests.cpp
```

## 最新代码说明：多线程 Reactor

这一轮代码把 Linux/macOS/select 路线从“单线程 Reactor”升级成了“多线程 Reactor”。

现在的线程分工是：

```text
主 EventLoop
    负责 Acceptor
    负责监听 listen socket
    负责 accept 新连接

worker EventLoop 1
    负责一部分客户端连接的 read/write

worker EventLoop 2
    负责另一部分客户端连接的 read/write

worker EventLoop N
    负责另一部分客户端连接的 read/write
```

新连接分发方式：

```text
第 1 个连接 -> worker 1
第 2 个连接 -> worker 2
第 3 个连接 -> worker 3
...
按轮询分发
```

核心原则：

```text
一个 Connection 只属于一个 worker EventLoop
读写事件都在这个 worker EventLoop 里处理
减少多个线程同时操作同一个连接的风险
```

这次主要修改：

```text
EventLoop 增加 run_in_loop() 任务队列
TcpServer 增加 worker EventLoop 线程池
新连接由主 EventLoop 接收
新连接按轮询投递给 worker EventLoop
Connection 在 worker EventLoop 中创建、读写、关闭
```

多线程 Reactor 当前流程：

```text
Acceptor accept 新连接
        ↓
TcpServer 选择一个 worker EventLoop
        ↓
worker EventLoop::run_in_loop()
        ↓
在 worker 线程里创建 Connection
        ↓
Connection 注册读事件
        ↓
worker EventLoop 负责这个连接后续 read/write
```

启动时可以指定 worker 数量：

```bash
./build/echo_server 0.0.0.0 8080 4
```

参数含义：

```text
第 1 个参数：监听地址
第 2 个参数：监听端口
第 3 个参数：worker_threads，0 表示自动使用 CPU 核数
```

如果不传第 3 个参数，默认是：

```text
worker_threads=0
自动使用 std::thread::hardware_concurrency()
```

当前验证结果：

```text
默认 kqueue 构建通过
select 后端构建通过
warnings-as-errors 严格构建通过
启动时显示 worker_threads=8
单连接 echo 测试通过
64 并发 echo 测试通过
```

第 5 阶段已经完成第一版；下一步已经继续进入第 7 阶段：定时器和连接超时回收。

## 最新代码说明：定时器和空闲连接回收

这一轮代码完成了第 7 阶段的第一版：小根堆定时器和空闲连接超时关闭。

为什么需要它：

```text
客户端断网或长时间不发数据
        ↓
连接对象还在服务器里
        ↓
fd 和内存一直占着
        ↓
连接越积越多，最终拖垮服务器
```

现在可以配置：

```text
idle_timeout_seconds=0  不启用空闲超时
idle_timeout_seconds>0  连接空闲超过这个秒数后关闭
```

核心新增：

```text
TimerQueue
    使用小根堆保存定时任务
    最早过期的任务永远在堆顶
    返回 TimerId，支持 cancel(timer_id) 取消未执行的任务

EventLoop
    每轮循环检查到期定时器
    根据最近定时器调整 poller.wait() 的等待时间

Connection
    每次 recv/send 成功后刷新最后活跃时间

TcpServer
    定时检查连接是否超过 idle_timeout_seconds
    超时后主动关闭连接
```

相关代码：

```text
include/cpp20_server/net/timer_queue.h
src/net/timer_queue.cpp
include/cpp20_server/net/event_loop.h
src/net/event_loop.cpp
include/cpp20_server/net/connection.h
src/net/connection.cpp
src/net/tcp_server.cpp
```

这一轮改进：

```text
TimerQueue::run_after() / run_every() 现在会返回 TimerId
TimerQueue::cancel(TimerId) 可以取消未执行的定时器
EventLoop 暴露 cancel_timer(TimerId)
```

注意：当前取消语义是“协作式取消”。如果回调已经开始执行，就不会强行中断正在执行的 C++ 代码，而是保证还没执行的定时器不会再执行。

启动时指定空闲超时：

```bash
./build/echo_server 0.0.0.0 8080 4 60
```

参数含义：

```text
第 1 个参数：监听地址
第 2 个参数：监听端口
第 3 个参数：worker_threads，0 表示自动使用 CPU 核数
第 4 个参数：idle_timeout_seconds，0 表示不启用空闲超时
```

## 最新代码说明：连接输入缓冲区

这一轮代码给每个 `Connection` 增加了输入缓冲区，解决 HTTP 的半包、粘包和同一个连接多个请求问题。

为什么要这么做：

```text
TCP 是字节流，不是消息队列
recv() 读到多少字节是不固定的
一次 recv() 不一定等于一个完整 HTTP 请求
```

真实网络里会出现三种情况：

```text
半包：
    一个 HTTP 请求分两次甚至多次到达
    第一次只收到请求头的一半，不能立刻解析

粘包：
    两个 HTTP 请求粘在同一次 recv() 里
    服务器要能从里面拆出多个请求

同连接多个请求：
    客户端不关闭 TCP 连接
    在同一个连接上连续发送多个 HTTP 请求
```

所以现在每个连接都有：

```text
input_   输入缓冲区，保存已经收到但还没解析完的数据
output_  输出缓冲区，保存已经生成但还没发送完的数据
```

处理流程：

```text
recv() 读到字节
        ↓
追加到 Connection::input_
        ↓
HTTP parser 尝试从 input_ 里解析完整请求
        ↓
如果请求不完整：继续留在 input_，等待下次 recv()
        ↓
如果请求完整：生成响应，写入 output_
        ↓
从 input_ 删除已经消费的请求字节
        ↓
继续尝试解析下一个请求
```

核心代码：

```text
include/cpp20_server/net/connection.h
src/net/connection.cpp
include/cpp20_server/net/tcp_server.h
src/net/tcp_server.cpp
include/cpp20_server/protocol/http.h
src/protocol/http.cpp
```

新增接口：

```cpp
server.set_stream_callback([](Buffer& input, Buffer& output) {
    handle_demo_http_stream(input, output);
});
```

`set_message_callback()` 仍然保留给 Echo 这种简单协议使用。HTTP 使用 `set_stream_callback()`，因为 HTTP 必须自己判断消息边界。

## 最新代码说明：日志、配置、指标

这一轮进入第 8 阶段，先补工程化基础能力。

新增模块：

```text
Config   读取简单 key=value 配置
Logger   异步日志，业务线程只把日志放进队列，后台线程负责写
Metrics  把 ServerStats 转成文本或 Prometheus 风格指标
```

为什么要做这些：

```text
配置文件：启动参数越来越多，不能一直靠命令行硬传
异步日志：高并发下不能让业务线程卡在磁盘写日志上
指标：服务器跑起来后，要知道连接数、读写字节数这些状态
```

配置文件示例：

```text
examples/server.conf
```

启动 HTTP Server：

```bash
./build/http_server --config examples/server.conf
```

配置项：

```text
host = 127.0.0.1
port = 8080
worker_threads = 4
idle_timeout_seconds = 30
backlog = 4096
max_events = 4096
log_level = info
log_console = true
log_file =
```

查看 HTTP 指标：

```bash
curl http://127.0.0.1:8080/metrics
```

返回内容是 Prometheus 风格文本，例如：

```text
cpp20_server_accepted_connections_total{backend="kqueue"} 10
cpp20_server_active_connections{backend="kqueue"} 2
cpp20_server_bytes_read_total{backend="kqueue"} 128
cpp20_server_bytes_written_total{backend="kqueue"} 256
```

核心代码：

```text
include/cpp20_server/base/config.h
src/base/config.cpp
include/cpp20_server/base/logger.h
src/base/logger.cpp
include/cpp20_server/base/metrics.h
src/base/metrics.cpp
examples/http_server.cpp
```

## 最新代码说明：最小 HTTP Server

这一轮代码完成了第 6 阶段 / 第 13 阶段的第一版：最小 HTTP 协议层和 HTTP Server 示例。

新增能力：

```text
解析 HTTP 请求行：GET /health HTTP/1.1
解析 HTTP Header：Host、Content-Length
解析 HTTP Body：根据 Content-Length 读取 body
生成 HTTP Response：状态码、Content-Length、Content-Type、Connection
```

当前示例路由：

```text
GET  /        返回 hello http
GET  /health  返回 ok
POST /echo    返回请求 body
其他路径       返回 404
```

当前路由已经封装成 `HttpRouter`，可以用 `method + path` 注册不同回调，后续加新接口不需要继续堆很多 `if/else`。

相关代码：

```text
include/cpp20_server/protocol/http.h
src/protocol/http.cpp
examples/http_server.cpp
```

启动 HTTP Server：

```bash
./build/http_server 0.0.0.0 8080 4 30
```

用 curl 测试：

```bash
curl http://127.0.0.1:8080/health
curl -X POST http://127.0.0.1:8080/echo --data 'hello http'
```

当前 HTTP 已经支持：

```text
完整请求一次到达
一个请求分多次到达
多个请求粘在一起到达
同一个 TCP 连接连续发送多个请求
```

如果你想先理解为什么这种结构能支撑高并发，可以先看：

```text
docs/HIGH_CONCURRENCY.md
```

如果你想按阶段从小白开始实现完整服务器，可以看：

```text
docs/LEARNING_ROADMAP.md
```

## 小白先看：epoll / kqueue / IOCP 是什么

写服务器时会同时连接很多客户端。如果不用这些机制，程序只能这样做：

```text
检查第 1 个连接有没有数据
检查第 2 个连接有没有数据
检查第 3 个连接有没有数据
...
检查第 1000000 个连接有没有数据
```

这会非常慢。

所以现代服务器会把连接交给操作系统管理，然后问操作系统：

```text
现在到底哪些连接有事情要处理？
```

不同系统给出的高性能方案不一样：

```text
Linux    epoll   适合大量连接，是 Linux 高并发服务器常用方案
macOS    kqueue  macOS/BSD 的高性能事件通知机制
Windows  IOCP    Windows 上真正适合大量并发连接的完成端口模型
select   兼容性好，但性能一般，不适合百万连接
```

简单理解：

```text
epoll/kqueue：操作系统告诉你“哪个 socket 可以读/写了”
IOCP：操作系统告诉你“你之前提交的异步读/写已经完成了”
```

所以 Windows 的 `IOCP` 不是简单替换 `epoll` 函数名，它的代码结构本来就不同。

## 快速启动

### macOS / Linux

构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

启动服务器：

```bash
./build/echo_server 0.0.0.0 8080
```

启动 HTTP Server：

```bash
./build/http_server 0.0.0.0 8080
```

指定 worker 线程数：

```bash
./build/echo_server 0.0.0.0 8080 4
```

指定 worker 线程数和空闲超时时间：

```bash
./build/echo_server 0.0.0.0 8080 4 60
```

HTTP Server 也支持同样参数：

```bash
./build/http_server 0.0.0.0 8080 4 30
```

本机测试：

```bash
nc 127.0.0.1 8080
```

输入任意内容，服务端会原样返回。

也可以直接用一条命令测试：

```bash
printf 'hello server\n' | nc -w 1 127.0.0.1 8080
```

如果返回：

```text
hello server
```

说明服务器启动成功。

HTTP 本机测试：

```bash
curl http://127.0.0.1:8080/health
curl -X POST http://127.0.0.1:8080/echo --data 'hello http'
```

### Windows PowerShell

构建：

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

使用 Visual Studio 生成器时也可以：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

启动服务器：

```powershell
.\build\Release\echo_server.exe 0.0.0.0 8080
```

启动 HTTP Server：

```powershell
.\build\Release\http_server.exe 0.0.0.0 8080
```

指定 worker 线程数：

```powershell
.\build\Release\echo_server.exe 0.0.0.0 8080 4
```

指定 worker 线程数和空闲超时时间：

```powershell
.\build\Release\echo_server.exe 0.0.0.0 8080 4 60
```

HTTP Server 指定 worker 线程数和空闲超时时间：

```powershell
.\build\Release\http_server.exe 0.0.0.0 8080 4 30
```

如果使用单配置生成器，比如 MinGW，也可能是：

```powershell
.\build\echo_server.exe 0.0.0.0 8080
```

## 功能测试

当前已经加入第一版自动化测试，入口文件：

```text
tests/server_tests.cpp
```

构建并运行测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

也可以直接运行测试程序，看到每个测试项：

```bash
./build/server_tests
```

当前测试覆盖：

```text
TimerQueue 单次定时任务和重复定时任务
EventLoop 跨线程唤醒
Buffer 基础读写
Config 配置解析
AsyncLogger 异步写日志
Metrics 指标格式化
HTTP 请求解析
HTTP 响应生成和路由
HTTP 半包处理
HTTP 粘包处理
同一个 TCP 连接多个 HTTP 请求
真实 TcpServer 单连接 Echo
真实 TcpServer HTTP GET /health
真实 TcpServer HTTP POST /echo
真实 TcpServer 64 客户端并发 Echo
空闲连接 1 秒超时关闭
worker_threads=2 的启动和回显
worker_threads=4 的并发回显
```

本机 macOS 当前测试结果：

```text
默认 kqueue 后端：100% tests passed, 0 tests failed out of 1
select 后端：100% tests passed, 0 tests failed out of 1
warnings-as-errors 严格构建：100% tests passed, 0 tests failed out of 1
```

直接运行 `./build/server_tests` 的输出示例：

```text
[PASS] timer_queue (5 ms)
[PASS] buffer (0 ms)
[PASS] config_parser (0 ms)
[PASS] async_logger (1 ms)
[PASS] metrics_formatter (0 ms)
[PASS] http_parser (0 ms)
[PASS] http_stream (0 ms)
[PASS] http_response_routes (0 ms)
listening on 127.0.0.1:52182 backend=kqueue worker_threads=2
[PASS] single_connection_echo (102 ms)
listening on 127.0.0.1:52185 backend=kqueue worker_threads=2
[PASS] http_server_health (102 ms)
listening on 127.0.0.1:52188 backend=kqueue worker_threads=2
[PASS] http_server_post_echo (102 ms)
listening on 127.0.0.1:52191 backend=kqueue worker_threads=2
[PASS] http_server_partial_request (157 ms)
listening on 127.0.0.1:52195 backend=kqueue worker_threads=2
[PASS] http_server_sticky_requests (103 ms)
listening on 127.0.0.1:52199 backend=kqueue worker_threads=2
[PASS] http_server_same_connection (102 ms)
listening on 127.0.0.1:52202 backend=kqueue worker_threads=4
[PASS] concurrent_echo (103 ms)
listening on 127.0.0.1:52268 backend=kqueue worker_threads=2
[PASS] idle_timeout (1102 ms)
All tests passed.
```

这轮测试还发现并修复了一个兼容性问题：`select` 后端在客户端半关闭写端后会把 EOF 报成可读事件，如果先处理读事件，可能导致已经准备好的回包来不及发送。现在 `Channel::handle_event()` 会优先处理写事件，再处理读事件。

## 压测脚本

这一轮新增 HTTP 压测脚本：

```text
tools/bench_http.py
```

先启动服务器：

```bash
./build/http_server --config examples/server.conf 127.0.0.1 18080 4 30
```

再运行压测：

```bash
python3 tools/bench_http.py --host 127.0.0.1 --port 18080 --path /health -c 50 -n 5000
```

脚本会输出：

```text
configured_concurrency  本次设置的并发 TCP 连接数
successful_requests     成功请求数
failed_requests         失败请求数
error_rate_percent      错误率
qps_success             每秒成功请求数
latency_ms              min/avg/p50/p90/p95/p99/max 延迟
status_codes            HTTP 状态码分布
errors                  错误类型统计
```

更详细说明见：

```text
docs/BENCHMARK.md
```

本机 macOS / kqueue / 127.0.0.1 首轮压测结果：

```text
命令：
python3 tools/bench_http.py --host 127.0.0.1 --port 18080 --path /health -c 50 -n 5000 --fail-on-error

configured_concurrency: 50
requested_requests: 5000
successful_requests: 5000
failed_requests: 0
error_rate_percent: 0.00
elapsed_seconds: 0.154
qps_success: 32409.77
latency_ms avg: 1.500
latency_ms p50: 1.265
latency_ms p90: 1.577
latency_ms p95: 1.702
latency_ms p99: 2.412
latency_ms max: 22.472
status_codes: 200=5000
errors: none
```

## 指定 IO 后端

默认情况下项目会自动选择当前系统最合适的后端：

```text
Linux    -> epoll
macOS    -> kqueue
Windows  -> iocp
```

也可以手动指定：

```bash
cmake -S . -B build -DCPP20_SERVER_BACKEND=epoll
cmake -S . -B build -DCPP20_SERVER_BACKEND=kqueue
cmake -S . -B build -DCPP20_SERVER_BACKEND=iocp
cmake -S . -B build -DCPP20_SERVER_BACKEND=select
```

注意：

```text
epoll  只能在 Linux 使用
kqueue 当前只在 macOS 开启
iocp   只能在 Windows 使用
select 兼容性最好，但不适合百万连接
```

## 百万级并发说明

想支持百万连接，代码层面必须避免“一个连接一个线程”。本项目使用的方向是：

```text
Linux    epoll + 非阻塞 socket
macOS    kqueue + 非阻塞 socket
Windows  IOCP + 异步 WSARecv/WSASend
```

但是实际跑到百万连接还需要系统调优。

Linux 常见参数：

```bash
ulimit -n 1048576
sysctl -w net.core.somaxconn=65535
sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sysctl -w net.ipv4.ip_local_port_range="10000 65000"
```

Windows 常见检查项：

```text
使用 64 位程序
使用 IOCP，不使用 select
增大可用内存
准备多台压测客户端机器
检查动态端口范围
检查防火墙和杀毒软件是否限制连接
```

Windows 查看动态端口范围：

```powershell
netsh int ipv4 show dynamicport tcp
```

注意：单机自己连接自己通常测不到真实百万连接，因为客户端端口、内存和系统限制会先成为瓶颈。真正压测通常需要多台客户端机器。

## 示例：Echo Server

Echo Server 的逻辑很简单：

```text
客户端发送什么
        ↓
服务器读取数据
        ↓
服务器把相同数据写回客户端
```

入口文件：

```text
examples/echo_server.cpp
```

核心服务器实现：

```text
src/net/tcp_server.cpp
```

## 上传到 GitHub

如果是第一次上传到空仓库：

```bash
git init
git add .
git commit -m "Initial C++20 server project"
git branch -M main
git remote add origin https://github.com/zmy1213/server.git
git push -u origin main
```

如果已经添加过远程仓库：

```bash
git remote set-url origin https://github.com/zmy1213/server.git
git push -u origin main
```

如果 GitHub 要求登录，推荐使用 Personal Access Token，或者先配置 SSH key 后把远程地址改成：

```bash
git remote set-url origin git@github.com:zmy1213/server.git
```

## 后续开发计划

1. 增加 Linux 百万连接参数调优文档
2. 增加 Windows AcceptEx 批量异步接收连接
3. 增加 C++ HTTP Client 和异步 connect
4. 增加 GitHub Actions 跨平台构建测试

## 当前阶段说明

当前版本是学习和扩展用的第一版服务器骨架，重点是：

```text
先跑通跨平台构建
再跑通非阻塞 TCP 服务
再逐步升级到高并发架构
```

不要一开始就把所有功能写复杂。服务器开发的正确顺序是先保证能运行、能测试、结构清晰，然后再逐步加多线程、定时器、协议解析和性能优化。
