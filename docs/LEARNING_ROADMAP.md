# C++20 高并发服务器学习与实现路线

这份文档是项目的长期路线图。

目标不是一口气写完所有功能，而是按阶段推进：

```text
先写能跑的最小 TCP 服务器
再理解非阻塞 IO
再理解 epoll/kqueue/IOCP
再封装网络框架
再加多线程、协议、定时器、日志、压测和系统调优
```

每个阶段都应该做到：

```text
能编译
能运行
能测试
知道这一阶段解决了什么问题
知道下一阶段为什么需要出现
```

## 第 1 阶段：最小 TCP 服务器

先实现最基础的 TCP 流程：

```text
socket()
bind()
listen()
accept()
read() / recv()
write() / send()
close()
```

目标：

```text
理解客户端是怎么连进来的
理解服务端是怎么接收数据的
理解服务端是怎么返回数据的
```

最小流程：

```text
创建 socket
        ↓
绑定 IP 和端口
        ↓
开始监听
        ↓
等待客户端连接
        ↓
读取客户端数据
        ↓
把数据写回客户端
        ↓
关闭连接
```

这个阶段不追求高并发，只追求把 TCP 主线跑通。

## 第 2 阶段：非阻塞 IO

普通 socket 默认是阻塞的。

阻塞的意思是：

```text
客户端没发数据
recv() 就一直卡住
客户端收得慢
send() 也可能卡住
```

所以要把 socket 改成非阻塞。

Linux/macOS：

```cpp
fcntl(fd, F_SETFL, O_NONBLOCK);
```

Windows：

```cpp
ioctlsocket(fd, FIONBIO, &mode);
```

目标：

```text
服务器不会因为一个客户端慢，就卡住整个线程
recv/send 不能继续时，先返回，之后再处理
```

本项目相关代码：

```text
src/net/socket.cpp
set_non_blocking(socket_t fd)
```

## 第 3 阶段：事件通知机制

只有非阻塞 IO 还不够。

如果服务器有 100 万个连接，不能每次都这样：

```text
检查连接 1
检查连接 2
检查连接 3
...
检查连接 1000000
```

正确做法是让操作系统告诉我们：

```text
哪些连接现在可以读
哪些连接现在可以写
哪些连接已经关闭
```

不同平台使用不同机制：

```text
Linux    epoll
macOS    kqueue
Windows  IOCP
```

Linux epoll 核心函数：

```text
epoll_create1()
epoll_ctl()
epoll_wait()
```

目标：

```text
一个线程可以管理大量 socket
只处理真正活跃的连接
空闲连接不大量消耗 CPU
```

本项目相关代码：

```text
src/net/poller.cpp
```

## 第 4 阶段：封装 Reactor 模型

当前代码已经有初步封装，但后续要继续拆清楚。

当前项目状态：这一阶段已经完成第一版拆分。

已经新增：

```text
EventLoop
Channel
Acceptor
Connection
```

对应文件：

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

这一轮拆分的目的：

```text
TcpServer 不再负责所有细节
Acceptor 专门接收新连接
Connection 专门管理单个客户端
EventLoop 专门处理事件循环
Channel 专门把 fd 事件转成回调
```

这为第 5 阶段“多线程 Reactor”打基础。

目标结构：

```text
EventLoop      事件循环，内部调用 epoll_wait / kevent
Channel        封装一个 fd 关心的事件，比如可读、可写
Poller         封装 epoll/kqueue/select
TcpServer      服务器入口
Acceptor       专门负责 accept 新连接
Connection     每个客户端连接的状态
Buffer         读缓冲区和写缓冲区
```

为什么要拆：

```text
TcpServer 不应该什么都管
EventLoop 只负责事件循环
Acceptor 只负责新连接
Connection 只负责单个客户端
Buffer 只负责数据缓存
```

拆完后，代码会更适合进入多线程阶段。

## 第 5 阶段：多线程 Reactor

单线程事件循环能处理很多连接，但不能充分利用多核 CPU。

当前项目状态：这一阶段已经完成第一版。

已经实现：

```text
主 EventLoop 负责 Acceptor
worker EventLoop 负责客户端连接读写
新连接按轮询分发给 worker EventLoop
Connection 在所属 worker EventLoop 中创建
Connection 后续 read/write/close 都在所属 worker 中处理
```

对应代码：

```text
include/cpp20_server/net/event_loop.h
src/net/event_loop.cpp
src/net/tcp_server.cpp
```

本阶段新增的关键能力：

```text
EventLoop::run_in_loop()
worker EventLoop 线程池
round-robin 新连接分发
启动参数 worker_threads
```

多线程结构：

```text
main thread
    负责监听端口
    负责 accept 新连接

worker thread 1
    EventLoop 1
    负责一部分连接的读写

worker thread 2
    EventLoop 2
    负责一部分连接的读写

worker thread N
    EventLoop N
    负责一部分连接的读写
```

连接分发策略：

```text
轮询分发
最少连接数分发
按 fd hash 分发
```

新手先做轮询分发。

核心原则：

```text
一个连接只属于一个 EventLoop 线程
不要让多个线程同时操作同一个连接
减少锁
减少数据竞争
```

## 第 6 阶段：协议层

网络框架跑通后，要开始支持协议。

建议顺序：

```text
Echo Server
        ↓
简单 HTTP Server
        ↓
带业务处理的 HTTP Server
        ↓
自定义二进制协议
```

Echo 协议：

```text
收到什么
返回什么
```

HTTP 协议：

```text
解析请求行
解析 header
解析 body
生成 HTTP response
```

当前项目状态：HTTP 协议层已经完成第一版。

已经新增：

```text
HttpRequest
parse_http_request()
make_http_response()
handle_demo_http_request()
http_server 示例
```

对应文件：

```text
include/cpp20_server/protocol/http.h
src/protocol/http.cpp
examples/http_server.cpp
```

当前支持的示例路由：

```text
GET  /        返回 hello http
GET  /health  返回 ok
POST /echo    返回请求 body
其他路径       返回 404
```

启动方式：

```bash
./build/http_server 0.0.0.0 8080 4 30
```

测试方式：

```bash
curl http://127.0.0.1:8080/health
curl -X POST http://127.0.0.1:8080/echo --data 'hello http'
```

当前项目状态：每个 `Connection` 已经增加输入缓冲区，HTTP 已支持半包、粘包和同一连接多个请求。

为什么要输入缓冲区：

```text
TCP 是字节流
recv() 不保证一次读到一个完整请求
HTTP 协议需要自己判断一条请求从哪里开始、到哪里结束
```

没有输入缓冲区会出问题：

```text
半包：
    第一次 recv 只读到请求头一半
    如果马上解析，就会误判请求错误

粘包：
    一次 recv 读到两个请求
    如果只处理一个，第二个请求会丢失

同连接多个请求：
    客户端复用 TCP 连接继续发请求
    服务器必须保留这个连接上的未解析数据
```

当前做法：

```text
Connection::input_ 保存收到但未解析完成的数据
HTTP parser 解析出一个完整请求后返回 bytes_consumed
Connection 从 input_ 删除已消费字节
如果 input_ 里还有数据，继续尝试解析下一个请求
```

对应接口：

```text
TcpServer::set_stream_callback()
handle_demo_http_stream()
try_parse_http_request()
```

自定义二进制协议：

```text
4 字节长度 + body
```

为什么需要协议层：

```text
TCP 只是字节流
TCP 不知道一条消息从哪里开始、从哪里结束
协议层负责解决拆包、粘包、消息边界
```

## 第 7 阶段：定时器

百万连接必须处理死连接。

否则会出现：

```text
客户端断网了
服务端不知道
连接对象一直占着内存
fd 一直占着
最后连接越来越多，服务器被拖死
```

需要实现：

```text
连接超时关闭
心跳检测
定时任务
延迟任务
```

新手先用小根堆定时器：

```text
每个定时任务有一个 expire_time
堆顶是最早过期的任务
EventLoop 每次循环检查堆顶是否过期
```

后续可以升级时间轮：

```text
TimerWheel
适合大量连接的心跳和超时管理
```

当前项目状态：这一阶段已经完成第一版。

已经新增：

```text
TimerQueue
连接最后活跃时间
idle_timeout_seconds 配置
空闲连接超时关闭
```

对应文件：

```text
include/cpp20_server/net/timer_queue.h
src/net/timer_queue.cpp
include/cpp20_server/net/event_loop.h
src/net/event_loop.cpp
include/cpp20_server/net/connection.h
src/net/connection.cpp
include/cpp20_server/net/tcp_server.h
src/net/tcp_server.cpp
```

当前实现方式：

```text
TimerQueue 使用小根堆保存定时任务
EventLoop 每轮循环先执行到期定时器
Connection 每次 recv/send 成功后刷新 last_active_at
TcpServer 给连接安排空闲检查任务
连接空闲超过 idle_timeout_seconds 后关闭
```

启动示例：

```bash
./build/echo_server 0.0.0.0 8080 4 60
```

参数含义：

```text
0.0.0.0  监听所有网卡
8080     监听端口
4        worker 线程数
60       空闲 60 秒自动关闭连接
```

当前测试覆盖：

```text
TimerQueue 单次定时任务
TimerQueue 重复定时任务
空闲连接 1 秒超时关闭
```

## 第 8 阶段：日志、配置、指标

工程化服务器必须有这些模块。

日志：

```text
Logger
异步日志队列
日志线程
日志文件滚动
```

配置：

```text
Config
监听地址
监听端口
worker 线程数
最大连接数
超时时间
日志级别
```

指标：

```text
当前连接数
总连接数
QPS
每秒读字节数
每秒写字节数
错误数
平均延迟
P99 延迟
```

信号：

```text
SIGINT
SIGTERM
优雅退出
```

## 第 9 阶段：性能优化

先跑通功能，再优化。

优化方向：

```text
减少内存拷贝
减少锁
减少系统调用
减少动态内存分配
连接对象复用
Buffer 复用
异步日志
批量 accept
批量 read/write
合理设置 TCP 参数
```

不要一开始就优化。

推荐顺序：

```text
先测出瓶颈
再针对瓶颈优化
优化后再次压测验证
```

## 第 10 阶段：系统调优

百万连接不仅是代码问题，还需要系统参数支持。

Linux 常见参数：

```bash
ulimit -n 1048576
sysctl -w net.core.somaxconn=65535
sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sysctl -w net.ipv4.ip_local_port_range="10000 65000"
sysctl -w net.ipv4.tcp_fin_timeout=15
sysctl -w net.ipv4.tcp_tw_reuse=1
```

重要理解：

```text
一个 TCP 连接在 Linux 里通常占用一个 fd
fd 数量不够，连接数就上不去
listen 队列太小，高峰连接会被拒绝
客户端端口范围太小，单机压测也上不去
```

Windows 常见检查项：

```text
使用 64 位程序
使用 IOCP
不要用 select 做百万连接
检查动态端口范围
检查防火墙和杀毒软件限制
准备足够内存
```

Windows 查看动态端口范围：

```powershell
netsh int ipv4 show dynamicport tcp
```

## 第 11 阶段：压测

服务器写完不等于高并发，必须压测。

要测：

```text
最大连接数
每秒新建连接数
QPS
平均延迟
P99 延迟
P999 延迟
CPU 使用率
内存使用量
错误率
连接断开恢复能力
```

常用工具：

```text
wrk
hey
ab
iperf
自写 TCP 压测客户端
```

注意：

```text
单台客户端机器通常压不出百万连接
客户端自己也会被端口、内存、CPU 限制
真实百万连接通常需要多台压测机
```

## 第 12 阶段：Windows IOCP 完善

当前项目已经有 IOCP 主干：

```text
CreateIoCompletionPort
WSARecv
WSASend
GetQueuedCompletionStatus
worker 线程
```

但还需要继续完善：

```text
AcceptEx 异步接收连接
连接对象池
发送队列
多 outstanding read/write
更完整的错误处理
Windows 实机编译测试
Windows 并发压测
```

为什么要 `AcceptEx`：

```text
当前 accept() 是独立线程同步接收连接
AcceptEx 可以让接收连接也变成 IOCP 完成事件
高连接建立速率下更合适
```

## 第 13 阶段：HTTP Server

Echo Server 只能证明网络层能跑。

当前项目状态：这一阶段已经完成第一版。

已经支持：

```text
GET /
GET /health
POST /echo
```

HTTP 最小响应：

```text
HTTP/1.1 200 OK
Content-Length: 12
Content-Type: text/plain

hello server
```

目标：

```text
理解协议解析
理解请求和响应
让服务器可以被浏览器和 curl 访问
```

对应代码：

```text
include/cpp20_server/protocol/http.h
src/protocol/http.cpp
examples/http_server.cpp
```

## 第 14 阶段：测试体系

需要增加自动化测试。

测试类型：

```text
单元测试
集成测试
回归测试
压力测试
跨平台构建测试
```

重点测试：

```text
Buffer 读写
协议解析
连接关闭
半包粘包
大量短连接
大量长连接
慢客户端
异常断开
```

当前项目状态：这一阶段已经完成第一版自动化测试。

已加入测试入口：

```text
tests/server_tests.cpp
```

当前测试覆盖：

```text
TimerQueue 单次定时任务和重复定时任务
Buffer 基础读写
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

运行方式：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

当前本机验证结果：

```text
默认 macOS kqueue 后端通过
select 后端通过
warnings-as-errors 严格构建通过
```

这轮测试发现并修复了一个 `select` 后端兼容性问题：

```text
客户端发送数据后 shutdown 写端
select 会把 EOF 报成可读事件
如果 Channel 先处理 read，可能先关闭连接，导致待发送数据没有写回客户端
现在 Channel::handle_event() 改成先处理 write，再处理 read
```

## 第 15 阶段：CI/CD

GitHub 上建议加 GitHub Actions。

至少测试：

```text
Ubuntu 构建
macOS 构建
Windows 构建
```

目标：

```text
每次 push 自动编译
防止改坏跨平台代码
尽早发现 Windows 编译错误
```

## 第 16 阶段：生产化功能

如果要变成真正可部署服务，需要继续加：

```text
配置文件
日志目录
优雅退出
健康检查
版本信息
运行指标
错误码
限流
最大连接数保护
黑名单
TLS
守护进程或服务化
```

这些不是高并发核心，但是真实服务必须有。

## 第 17 阶段：内存管理

百万连接下，内存非常关键。

需要关注：

```text
每个连接对象多大
每个 Buffer 默认多大
是否为每个连接预分配大 buffer
是否能按需扩容
是否能复用内存
是否能减少碎片
```

优化方向：

```text
ConnectionPool
BufferPool
对象复用
小对象分配器
按连接活跃程度分配 buffer
```

## 第 18 阶段：安全与健壮性

高并发服务器还要防异常输入。

需要处理：

```text
超大包
非法协议
慢速攻击
大量空闲连接
大量半开连接
短时间大量新连接
客户端异常断开
send/recv 各类错误码
```

保护策略：

```text
最大请求大小
最大连接数
读超时
写超时
协议解析失败立即断开
连接速率限制
```

## 第 19 阶段：跨平台抽象收敛

Linux/macOS 和 Windows 的模型不同：

```text
Linux/macOS  更偏 Reactor
Windows IOCP 更偏 Proactor
```

后续要把上层接口设计成统一的：

```text
Connection::send()
Connection::close()
TcpServer::set_message_callback()
TcpServer::start()
TcpServer::stop()
```

上层业务不应该关心底层是：

```text
epoll
kqueue
IOCP
select
```

## 第 20 阶段：最终项目形态

最终项目可以演进成：

```text
cpp20-server/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── HIGH_CONCURRENCY.md
│   ├── LEARNING_ROADMAP.md
│   ├── REACTOR.md
│   ├── IOCP.md
│   └── PERFORMANCE_TUNING.md
├── include/
│   └── cpp20_server/
│       ├── base/
│       │   ├── logger.h
│       │   ├── config.h
│       │   └── noncopyable.h
│       ├── net/
│       │   ├── event_loop.h
│       │   ├── channel.h
│       │   ├── acceptor.h
│       │   ├── tcp_server.h
│       │   ├── connection.h
│       │   ├── buffer.h
│       │   └── poller.h
│       ├── timer/
│       │   └── timer_queue.h
│       └── protocol/
│           ├── http_parser.h
│           └── length_header_codec.h
├── src/
├── examples/
│   ├── echo_server.cpp
│   ├── http_server.cpp
│   └── chat_server.cpp
└── tests/
```

## 当前项目下一步建议

当前项目已经完成：

```text
CMake 工程
跨平台 socket 封装
macOS kqueue 后端
Linux epoll 后端代码
Windows IOCP 主干代码
Echo Server
HTTP Server 第一版
HTTP 请求解析和响应生成第一版
Connection 输入缓冲区第一版
HTTP 半包、粘包、同连接多请求处理第一版
EventLoop / Channel / Acceptor / Connection 拆分
多线程 Reactor 第一版
TimerQueue 小根堆定时器第一版
空闲连接超时关闭第一版
自动化功能测试第一版
高并发原理文档
流程到代码定位文档
```

下一步最合理的是：

```text
第 8 阶段：日志、配置、指标
```

也就是先做工程化基础能力：

```text
Logger
Config
Metrics
```

这些做完后，再进入：

```text
压测
系统调优
```

不要跳着做。否则后面功能越多，代码越难维护。
