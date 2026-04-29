# 高并发服务器原理说明

这份文档解释本项目为什么要使用：

```text
非阻塞 socket
epoll / kqueue / IOCP
事件循环
缓冲区
少量线程处理大量连接
```

目标是让小白能理解：为什么这样写比“一个连接一个线程”更适合高并发。

## 先说结论

高并发服务器的核心思想是：

```text
不要给每个连接分配一个线程
不要让线程卡在 read / write 上
让操作系统告诉我们哪些连接真的有事情要处理
程序只处理活跃连接
```

也就是说，如果有 100 万个客户端连接在线，但某一瞬间只有 1000 个连接真的在发数据，服务器不应该傻傻检查 100 万个连接，也不应该创建 100 万个线程。

正确做法是：

```text
100 万个连接交给操作系统管理
操作系统发现哪些连接有事件
服务器只处理有事件的连接
```

这就是 `epoll`、`kqueue`、`IOCP` 这些机制存在的原因。

## 传统写法为什么不行

最容易想到的服务器写法是：

```text
来一个客户端连接
创建一个线程
这个线程专门负责这个客户端
```

伪代码类似：

```cpp
while (true) {
    int client = accept(listen_fd);
    std::thread([client] {
        while (true) {
            read(client);
            write(client);
        }
    }).detach();
}
```

这个写法简单，但高并发时会出大问题。

## 问题 1：线程太多

每个线程都需要内存栈空间。假设一个线程栈是 1 MB：

```text
1 万线程   大约 10 GB 栈空间
10 万线程  大约 100 GB 栈空间
100 万线程 大约 1000 GB 栈空间
```

这还没算 socket 缓冲区、连接对象、业务内存。

所以“一个连接一个线程”不适合百万连接。

## 问题 2：阻塞 IO 会卡住线程

普通 `read()` / `recv()` 默认是阻塞的。

意思是：

```text
如果客户端没有发数据
recv() 就一直等
线程就卡在那里
```

如果 100 万连接里大部分连接都没发数据，就会有大量线程只是卡着不动，浪费内存和调度成本。

## 问题 3：线程切换很贵

线程多了以后，操作系统要不停切换线程：

```text
保存线程 A 的状态
切换到线程 B
保存线程 B 的状态
切换到线程 C
...
```

连接越多，调度成本越高。

高并发服务器要尽量避免这种浪费。

## 本项目的做法

本项目当前方向是：

```text
Linux    epoll + 非阻塞 socket
macOS    kqueue + 非阻塞 socket
Windows  IOCP + 异步 WSARecv / WSASend
```

核心文件：

```text
include/cpp20_server/net/socket.h       socket 跨平台封装
include/cpp20_server/net/poller.h       epoll/kqueue/select 的统一事件接口
include/cpp20_server/net/event_loop.h   Reactor 事件循环
include/cpp20_server/net/channel.h      fd 事件和回调封装
include/cpp20_server/net/acceptor.h     新连接接收器
include/cpp20_server/net/connection.h   单个客户端连接状态
src/net/poller.cpp                      epoll/kqueue/select 的具体实现
src/net/tcp_server.cpp                  TCP 服务器主逻辑，Windows IOCP 也在这里
src/net/event_loop.cpp                  Reactor 事件分发
src/net/acceptor.cpp                    accept 新连接
src/net/connection.cpp                  recv/send 读写逻辑
include/cpp20_server/net/buffer.h       输出缓冲区
src/net/buffer.cpp                      缓冲区实现
```

## 本轮代码在做什么：Reactor 拆分

这一轮代码是在做“第 4 阶段：封装 Reactor 模型”。

它不是新增 HTTP、定时器、日志这些业务功能，而是把原来集中在 `TcpServer` 里的网络逻辑拆成几个更小的模块。

拆分前，`TcpServer` 同时负责：

```text
创建监听 socket
accept 新连接
等待 epoll/kqueue 事件
判断 fd 是可读还是可写
recv() 读取客户端数据
send() 返回响应
保存每个连接的状态
关闭连接并清理资源
```

这样继续往下写会有一个问题：

```text
所有逻辑都堆在 TcpServer 里
后面加多线程、定时器、HTTP、压测时会越来越乱
```

所以这一轮把非 Windows IOCP 路线拆成：

```text
TcpServer
    服务器入口
    负责创建 EventLoop 和 Acceptor
    负责保存所有 Connection

EventLoop
    事件循环
    内部调用 Poller::wait()
    把事件分发给 Channel

Channel
    封装一个 fd
    保存这个 fd 关心 read/write/close/error 哪些事件
    事件发生后调用对应回调

Acceptor
    只负责监听 socket
    只负责 accept 新连接
    accept 成功后把 client socket 交给 TcpServer

Connection
    只负责一个客户端连接
    负责 recv()
    负责 send()
    负责输出 Buffer
    负责关闭连接

Poller
    封装 epoll/kqueue/select
    只负责和操作系统事件通知机制打交道
```

拆分后的主线变成：

```text
TcpServer::start()
        ↓
创建 Acceptor
        ↓
进入 EventLoop::loop()
        ↓
Poller::wait() 等待事件
        ↓
EventLoop 找到对应 Channel
        ↓
Channel 调用回调
        ↓
如果是 listen fd 可读：Acceptor::handle_read()
        ↓
accept() 得到 client socket
        ↓
TcpServer 创建 Connection
        ↓
如果是 client fd 可读：Connection::handle_read()
        ↓
业务回调生成响应
        ↓
Connection::handle_write() 发送响应
```

这一轮的意义：

```text
代码职责更清楚
每个类只做一件主要的事
为多线程 Reactor 做准备
以后一个 Connection 可以绑定到某个 EventLoop
以后 Acceptor 可以只负责接收连接，worker EventLoop 负责读写
```

## 当前代码在做什么：多线程 Reactor

在 Reactor 拆分之后，项目继续完成了“第 5 阶段：多线程 Reactor”的第一版。

现在 Linux/macOS/select 路线不是所有连接都在一个 `EventLoop` 里处理，而是：

```text
主 EventLoop
    负责 Acceptor
    负责 accept 新连接

worker EventLoop
    负责客户端连接 read/write/close
```

新连接进来后的分发方式：

```text
Acceptor accept 新连接
        ↓
TcpServer 按轮询选择 worker EventLoop
        ↓
EventLoop::run_in_loop() 投递任务
        ↓
worker 线程创建 Connection
        ↓
这个 Connection 后续只归这个 worker 管理
```

对应代码：

| 功能 | 代码位置 | 说明 |
| --- | --- | --- |
| `EventLoop` 任务队列 | [`include/cpp20_server/net/event_loop.h`](../include/cpp20_server/net/event_loop.h), [`src/net/event_loop.cpp`](../src/net/event_loop.cpp) | `run_in_loop()` 支持从主 loop 向 worker loop 投递任务 |
| 启动 worker loop | [`src/net/tcp_server.cpp`](../src/net/tcp_server.cpp) | `start_worker_loops()` 创建多个 `EventLoop` 和线程 |
| 轮询选择 worker | [`src/net/tcp_server.cpp`](../src/net/tcp_server.cpp) | `next_worker_loop()` 用 round-robin 分发新连接 |
| 在 worker 中创建连接 | [`src/net/tcp_server.cpp`](../src/net/tcp_server.cpp) | `create_connection()` 在目标 worker loop 里创建 `Connection` |

这个阶段解决的问题：

```text
单个 EventLoop 只能使用一个 CPU 核
多线程 Reactor 可以让多个 worker EventLoop 同时处理不同连接
一个连接固定属于一个 worker，避免多个线程同时读写同一个连接
```

当前实现还有一个后续优化点：

```text
EventLoop::run_in_loop() 现在用任务队列加短轮询等待
后面可以加 eventfd/socketpair 唤醒机制
这样跨线程投递任务时可以立即唤醒目标 worker
```

## 第一步：socket 设置成非阻塞

非阻塞 socket 的意思是：

```text
recv() 没有数据时，不要卡住线程
send() 暂时写不出去时，不要卡住线程
直接返回一个 would block 错误
```

这样服务器线程就不会被某个慢客户端拖死。

代码位置：

```text
src/net/socket.cpp
set_non_blocking(socket_t fd)
```

Linux/macOS 上核心是：

```cpp
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

Windows 上核心是：

```cpp
ioctlsocket(fd, FIONBIO, &mode);
```

小白理解：

```text
阻塞 socket：没数据就一直等
非阻塞 socket：没数据就立刻返回，服务器可以去处理别的连接
```

## 第二步：用操作系统事件机制

如果只有非阻塞 socket，还不够。

因为服务器仍然可能需要这样做：

```text
检查连接 1 有没有数据
检查连接 2 有没有数据
检查连接 3 有没有数据
...
检查连接 1000000 有没有数据
```

这还是很慢。

所以我们要把连接交给操作系统，让操作系统告诉我们：

```text
哪些连接现在可以读
哪些连接现在可以写
哪些连接已经关闭
哪些连接出错
```

不同系统的机制不同：

```text
Linux    epoll
macOS    kqueue
Windows  IOCP
```

## Linux：epoll 的原理

`epoll` 可以简单理解为：

```text
服务器把很多 socket 注册给 epoll
epoll_wait() 睡眠等待
某些 socket 有数据或可写时，epoll_wait() 返回这些 socket
服务器只处理返回的 socket
```

流程：

```text
epoll_create1()
        ↓
epoll_ctl() 添加 socket
        ↓
epoll_wait() 等事件
        ↓
处理可读 / 可写 socket
```

本项目代码位置：

```text
src/net/poller.cpp
CPP20_SERVER_USE_EPOLL 分支
```

核心函数：

```text
epoll_create1
epoll_ctl
epoll_wait
```

为什么适合高并发：

```text
连接很多时，服务器不用每次扫描所有连接
epoll_wait() 只返回真正有事件的连接
空闲连接几乎不消耗用户态 CPU
```

## macOS：kqueue 的原理

`kqueue` 是 macOS/BSD 的高性能事件机制。

它和 `epoll` 的思想接近：

```text
把 socket 注册给 kqueue
用 kevent() 等待事件
事件来了再处理
```

流程：

```text
kqueue()
        ↓
kevent() 注册 EVFILT_READ / EVFILT_WRITE
        ↓
kevent() 等待事件
        ↓
处理可读 / 可写 socket
```

本项目代码位置：

```text
src/net/poller.cpp
CPP20_SERVER_USE_KQUEUE 分支
```

核心函数：

```text
kqueue
kevent
EVFILT_READ
EVFILT_WRITE
```

小白理解：

```text
epoll 是 Linux 的高并发事件机制
kqueue 是 macOS 的高并发事件机制
它们目的类似，只是系统 API 不同
```

## Windows：IOCP 的原理

Windows 上真正适合高并发连接的是 `IOCP`。

IOCP 和 epoll/kqueue 有一个重要区别：

```text
epoll/kqueue：告诉你 socket 现在可以读或写
IOCP：告诉你之前提交的异步读写已经完成
```

也就是说，IOCP 不是问：

```text
这个 socket 现在能不能读？
```

而是先提交一个异步读：

```text
WSARecv()
```

然后操作系统完成后通知你：

```text
这个读操作已经完成，读到了多少字节
```

本项目代码位置：

```text
src/net/tcp_server.cpp
CPP20_SERVER_USE_IOCP 分支
```

核心函数：

```text
CreateIoCompletionPort
GetQueuedCompletionStatus
WSARecv
WSASend
```

IOCP 流程：

```text
创建 IOCP
        ↓
socket 绑定到 IOCP
        ↓
提交 WSARecv 异步读
        ↓
worker 线程等待 GetQueuedCompletionStatus
        ↓
读完成后处理数据
        ↓
提交 WSASend 异步写
        ↓
写完成后继续提交下一次 WSARecv
```

为什么适合高并发：

```text
少量 worker 线程处理大量连接
线程不需要为每个连接阻塞等待
操作系统完成 IO 后再唤醒 worker
非常适合 Windows 大量连接场景
```

## Reactor 和 Proactor

这里有两个常见概念：

```text
Reactor
Proactor
```

本项目里：

```text
Linux epoll   更像 Reactor
macOS kqueue  更像 Reactor
Windows IOCP  更像 Proactor
```

## Reactor 是什么

Reactor 的意思是：

```text
操作系统告诉你：这个 socket 可以读了
服务器自己调用 recv() 去读
```

流程：

```text
等待事件
        ↓
发现 fd 可读
        ↓
服务器 recv()
        ↓
处理数据
        ↓
需要写时注册写事件
        ↓
发现 fd 可写
        ↓
服务器 send()
```

本项目 Linux/macOS 当前就是这个路线。

## Proactor 是什么

Proactor 的意思是：

```text
服务器先提交一个异步操作
操作系统完成后通知服务器
```

流程：

```text
提交异步读 WSARecv
        ↓
操作系统后台完成读取
        ↓
通知 worker：读完成
        ↓
服务器处理数据
        ↓
提交异步写 WSASend
        ↓
操作系统后台完成写入
        ↓
通知 worker：写完成
```

Windows IOCP 就是这个路线。

## 事件循环是什么

事件循环就是服务器的主循环。

Linux/macOS 下可以理解成：

```text
while (server_running) {
    events = poller.wait();
    for (event in events) {
        if (event 是新连接) {
            accept();
        }
        if (event 可读) {
            recv();
        }
        if (event 可写) {
            send();
        }
    }
}
```

本项目代码位置：

```text
src/net/tcp_server.cpp
TcpServer::Impl::start()
```

这段循环就是服务器不断处理事件的地方。

事件循环代码定位：

| 流程步骤 | 代码位置 | 说明 |
| --- | --- | --- |
| 示例程序调用服务器启动 | [`examples/echo_server.cpp:39-48`](../examples/echo_server.cpp#L39-L48) | 创建 `TcpServer`，设置 echo 回调，然后调用 `server.start()` |
| 对外 `TcpServer::start()` | [`src/net/tcp_server.cpp:493-495`](../src/net/tcp_server.cpp#L493-L495) | 公共入口，内部转发给平台相关的 `Impl::start()` |
| Linux/macOS 启动 Reactor | [`src/net/tcp_server.cpp:406-415`](../src/net/tcp_server.cpp#L406-L415) | 创建 `Acceptor`，设置新连接回调，然后进入 `EventLoop::loop()` |
| Linux/macOS 事件循环 | [`src/net/event_loop.cpp:13-23`](../src/net/event_loop.cpp#L13-L23) | `EventLoop` 循环等待事件，并把事件分发给对应 `Channel` |
| Channel 回调分发 | [`src/net/channel.cpp:77-96`](../src/net/channel.cpp#L77-L96) | `Channel` 根据 read/write/close/error 调用对应回调 |
| Windows IOCP 启动流程 | [`src/net/tcp_server.cpp:63-81`](../src/net/tcp_server.cpp#L63-L81) | 创建监听 socket，创建 IOCP，启动 worker 线程和 accept 线程 |
| Linux/macOS 等待事件 | [`src/net/poller.cpp:165-224`](../src/net/poller.cpp#L165-L224) | `Poller::wait()` 内部调用 `epoll_wait()` 或 `kevent()` |
| Windows 等待完成事件 | [`src/net/tcp_server.cpp:274-320`](../src/net/tcp_server.cpp#L274-L320) | worker 线程调用 `GetQueuedCompletionStatus()` 等待异步 IO 完成 |

## 为什么需要 Buffer

网络发送不是你想发多少就一定能一次发完。

比如你要发送 100 KB：

```text
send() 第一次可能只发出去 20 KB
剩下 80 KB 必须以后继续发
```

所以服务器需要一个输出缓冲区：

```text
没发完的数据先存起来
等 socket 再次可写
继续发送剩下的数据
```

本项目代码位置：

```text
include/cpp20_server/net/buffer.h
src/net/buffer.cpp
```

在 Linux/macOS Reactor 实现里，每个连接都有：

```cpp
Buffer output;
```

它的作用是保存还没发完的数据。

## 一个连接在本项目里的生命周期

Linux/macOS 流程：

```text
客户端连接进来
        ↓
listen_fd 触发可读事件
        ↓
Acceptor::handle_read()
        ↓
accept() 得到 client socket
        ↓
设置非阻塞
        ↓
注册到 Poller
        ↓
客户端发数据
        ↓
Poller 返回可读事件
        ↓
Connection::handle_read()
        ↓
调用业务回调生成响应
        ↓
写入 Buffer
        ↓
注册可写事件
        ↓
Connection::handle_write()
        ↓
send() 返回客户端
```

Linux/macOS 流程代码定位：

| 流程步骤 | 代码位置 | 说明 |
| --- | --- | --- |
| 创建 `Acceptor` | [`src/net/tcp_server.cpp:406-410`](../src/net/tcp_server.cpp#L406-L410) | `TcpServer` 创建 `Acceptor`，并设置新连接回调 |
| 创建监听 socket | [`src/net/acceptor.cpp:16-21`](../src/net/acceptor.cpp#L16-L21) | `Acceptor` 调用 `create_listening_socket()`，并把监听 fd 包装成 `Channel` |
| `socket/bind/listen` | [`src/net/socket.cpp:154-197`](../src/net/socket.cpp#L154-L197) | 创建 socket，绑定地址端口，开始监听，并设置非阻塞 |
| 事件循环等待事件 | [`src/net/event_loop.cpp:13-23`](../src/net/event_loop.cpp#L13-L23) | `EventLoop` 调用 `poller_.wait()` 等待操作系统返回活跃 socket |
| epoll 创建和等待 | [`src/net/poller.cpp:77-91`](../src/net/poller.cpp#L77-L91), [`src/net/poller.cpp:165-182`](../src/net/poller.cpp#L165-L182) | Linux 下创建 `epoll_fd_`，用 `epoll_wait()` 返回活跃连接 |
| kqueue 创建和等待 | [`src/net/poller.cpp:81-95`](../src/net/poller.cpp#L81-L95), [`src/net/poller.cpp:183-220`](../src/net/poller.cpp#L183-L220) | macOS 下创建 `kqueue_fd_`，用 `kevent()` 返回活跃连接 |
| 监听 socket 可读 | [`src/net/acceptor.cpp:20-21`](../src/net/acceptor.cpp#L20-L21), [`src/net/channel.cpp:77-83`](../src/net/channel.cpp#L77-L83) | 监听 fd 的 `Channel` 收到可读事件后调用 `Acceptor::handle_read()` |
| `accept()` 新连接 | [`src/net/acceptor.cpp:40-72`](../src/net/acceptor.cpp#L40-L72) | 循环 `accept()`，得到 `client socket` |
| 新连接设置非阻塞 | [`src/net/acceptor.cpp:59-63`](../src/net/acceptor.cpp#L59-L63), [`src/net/socket.cpp:72-90`](../src/net/socket.cpp#L72-L90) | 设置非阻塞和 `TCP_NODELAY`，然后回调给 `TcpServer` |
| 创建 `Connection` | [`src/net/tcp_server.cpp:435-452`](../src/net/tcp_server.cpp#L435-L452), [`src/net/connection.cpp:34-43`](../src/net/connection.cpp#L34-L43) | `TcpServer` 创建连接对象，`Connection` 绑定 read/write/close/error 回调 |
| 注册连接读事件 | [`src/net/connection.cpp:65-67`](../src/net/connection.cpp#L65-L67), [`src/net/channel.cpp:39-43`](../src/net/channel.cpp#L39-L43) | `Connection::start()` 让 `Channel` 关注读事件 |
| 普通连接可读 | [`src/net/channel.cpp:77-83`](../src/net/channel.cpp#L77-L83), [`src/net/connection.cpp:86-122`](../src/net/connection.cpp#L86-L122) | `Channel` 调用 `Connection::handle_read()`，`recv()` 读取客户端数据 |
| 生成响应并写入 Buffer | [`src/net/connection.cpp:90-99`](../src/net/connection.cpp#L90-L99) | 调用 `on_message_` 业务回调，响应数据写入输出缓冲区 |
| 注册可写事件 | [`src/net/connection.cpp:119-120`](../src/net/connection.cpp#L119-L120), [`src/net/channel.cpp:45-49`](../src/net/channel.cpp#L45-L49) | 如果输出缓冲区有数据，就让 `Channel` 同时关注写事件 |
| 普通连接可写 | [`src/net/channel.cpp:84-87`](../src/net/channel.cpp#L84-L87), [`src/net/connection.cpp:124-159`](../src/net/connection.cpp#L124-L159) | `Channel` 调用 `Connection::handle_write()`，`send()` 发送 Buffer 数据 |
| 发送完成后取消写事件 | [`src/net/connection.cpp:148-158`](../src/net/connection.cpp#L148-L158), [`src/net/channel.cpp:51-55`](../src/net/channel.cpp#L51-L55) | 如果 Buffer 清空，就不再关注写事件，只保留读事件 |
| 连接关闭或错误 | [`src/net/connection.cpp:161-177`](../src/net/connection.cpp#L161-L177), [`src/net/tcp_server.cpp:455-463`](../src/net/tcp_server.cpp#L455-L463) | 连接关闭时回调 `TcpServer` 删除连接对象 |

Windows IOCP 流程：

```text
客户端连接进来
        ↓
accept() 得到 client socket
        ↓
绑定到 IOCP
        ↓
post_recv() 提交异步读
        ↓
GetQueuedCompletionStatus() 等待读完成
        ↓
worker_loop() 处理数据
        ↓
post_send() 提交异步写
        ↓
写完成后继续 post_recv()
```

Windows IOCP 流程代码定位：

| 流程步骤 | 代码位置 | 说明 |
| --- | --- | --- |
| 示例程序调用服务器启动 | [`examples/echo_server.cpp:39-48`](../examples/echo_server.cpp#L39-L48) | Windows 和 Linux/macOS 一样，入口都是创建 `TcpServer` 后调用 `server.start()` |
| 对外 `TcpServer::start()` | [`src/net/tcp_server.cpp:493-495`](../src/net/tcp_server.cpp#L493-L495) | 公共入口，内部转发到 Windows IOCP 版本的 `Impl::start()` |
| 创建监听 socket | [`src/net/tcp_server.cpp:63-64`](../src/net/tcp_server.cpp#L63-L64), [`src/net/socket.cpp:154-197`](../src/net/socket.cpp#L154-L197) | 创建服务端监听 socket，内部完成 `socket/bind/listen` |
| 创建 IOCP 完成端口 | [`src/net/tcp_server.cpp:65-68`](../src/net/tcp_server.cpp#L65-L68) | `CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...)` 创建一个 IOCP 队列 |
| 启动 worker 线程 | [`src/net/tcp_server.cpp:70`](../src/net/tcp_server.cpp#L70), [`src/net/tcp_server.cpp:135-143`](../src/net/tcp_server.cpp#L135-L143) | 根据 CPU 核数或配置启动多个 worker，worker 后面负责处理 IO 完成事件 |
| 启动 accept 线程 | [`src/net/tcp_server.cpp:71`](../src/net/tcp_server.cpp#L71) | 单独线程执行 `accept_loop()`，负责接收新连接 |
| 等待客户端连接 | [`src/net/tcp_server.cpp:157-188`](../src/net/tcp_server.cpp#L157-L188) | `accept_loop()` 循环调用 `accept()` 获取新的 client socket |
| `accept()` 得到 client socket | [`src/net/tcp_server.cpp:161-163`](../src/net/tcp_server.cpp#L161-L163) | 新客户端连接进来后，`accept()` 返回这个客户端对应的 socket |
| 新连接进入注册流程 | [`src/net/tcp_server.cpp:180-182`](../src/net/tcp_server.cpp#L180-L182) | 设置 `TCP_NODELAY` 后调用 `register_connection(client)` |
| 创建连接对象 | [`src/net/tcp_server.cpp:190-193`](../src/net/tcp_server.cpp#L190-L193) | 为这个客户端创建 `Connection`，保存 fd、读上下文、写上下文和关闭状态 |
| 绑定 client socket 到 IOCP | [`src/net/tcp_server.cpp:195-201`](../src/net/tcp_server.cpp#L195-L201) | `CreateIoCompletionPort(socket, iocp_, key, 0)` 把 socket 关联到 IOCP |
| 保存连接并更新统计 | [`src/net/tcp_server.cpp:203-208`](../src/net/tcp_server.cpp#L203-L208) | 把连接放进 `connections_`，增加 accepted/active 计数 |
| 提交第一次异步读 | [`src/net/tcp_server.cpp:210`](../src/net/tcp_server.cpp#L210), [`src/net/tcp_server.cpp:213-241`](../src/net/tcp_server.cpp#L213-L241) | `post_recv()` 准备 `OVERLAPPED/WSABUF`，调用 `WSARecv()` 提交异步读 |
| worker 等待 IO 完成 | [`src/net/tcp_server.cpp:274-283`](../src/net/tcp_server.cpp#L274-L283) | worker 调用 `GetQueuedCompletionStatus()`，阻塞等待读或写完成 |
| 根据 completion key 找回连接 | [`src/net/tcp_server.cpp:288-292`](../src/net/tcp_server.cpp#L288-L292) | IOCP 返回之前绑定的 `Connection*`，再通过 `OVERLAPPED*` 找到本次操作上下文 |
| 读完成后处理数据 | [`src/net/tcp_server.cpp:301-310`](../src/net/tcp_server.cpp#L301-L310) | 如果是 `recv` 完成，统计读取字节，调用 `on_message_` 生成响应 |
| 提交异步写 | [`src/net/tcp_server.cpp:309`](../src/net/tcp_server.cpp#L309), [`src/net/tcp_server.cpp:243-272`](../src/net/tcp_server.cpp#L243-L272) | `post_send()` 把响应放入发送上下文，调用 `WSASend()` 提交异步写 |
| 写完成后继续读 | [`src/net/tcp_server.cpp:313-320`](../src/net/tcp_server.cpp#L313-L320) | 如果是 `send` 完成，统计写入字节，然后再次 `post_recv()` 等下一条消息 |
| 客户端关闭或 IO 错误 | [`src/net/tcp_server.cpp:294-298`](../src/net/tcp_server.cpp#L294-L298) | `GetQueuedCompletionStatus()` 失败或传输字节为 0 时，进入关闭流程 |
| 关闭连接 | [`src/net/tcp_server.cpp:332-342`](../src/net/tcp_server.cpp#L332-L342) | 标记连接关闭并关闭 socket |
| 没有未完成 IO 后释放连接 | [`src/net/tcp_server.cpp:324-330`](../src/net/tcp_server.cpp#L324-L330), [`src/net/tcp_server.cpp:344-358`](../src/net/tcp_server.cpp#L344-L358) | `pending_operations` 归零后，从 `connections_` 删除连接对象 |
| 停止服务器唤醒 worker | [`src/net/tcp_server.cpp:84-98`](../src/net/tcp_server.cpp#L84-L98) | `stop()` 关闭监听 socket，并用 `PostQueuedCompletionStatus()` 唤醒 worker 退出 |

注意：当前 Windows 版本为了先打通 IOCP 主干，accept 阶段仍使用独立线程里的 `accept()`。后续如果要进一步冲击百万连接，下一步应改成 `AcceptEx`，让接收新连接本身也变成 IOCP 异步完成事件。

## 为什么这样能支持更多连接

核心原因有四个。

## 原因 1：连接不等于线程

传统写法：

```text
1 个连接 = 1 个线程
100 万连接 = 100 万线程
```

本项目方向：

```text
1 个连接 = 1 个 socket + 1 个连接对象
很多连接 = 少量线程统一处理
```

这样内存压力和线程调度压力会小很多。

## 原因 2：空闲连接不消耗大量 CPU

高并发长连接场景下，大部分连接很多时候是空闲的。

比如：

```text
100 万连接在线
某一秒只有 5000 个连接发消息
```

好的服务器应该只处理这 5000 个活跃连接。

`epoll/kqueue/IOCP` 的作用就是让服务器只被活跃连接唤醒。

## 原因 3：非阻塞避免慢客户端拖死服务器

如果某个客户端很慢：

```text
读得慢
写得慢
网络差
暂时不收数据
```

服务器不能一直卡在这个客户端身上。

非阻塞 IO 的作用就是：

```text
能读就读
能写就写
不能读写就先放一边
等操作系统下次通知
```

## 原因 4：按事件处理，而不是盲目轮询

传统轮询：

```text
每次扫描所有连接
```

事件驱动：

```text
操作系统告诉你哪些连接有变化
```

连接数越大，事件驱动优势越明显。

## 但是百万连接不只靠代码

要跑到百万连接，还需要系统资源配合。

Linux 常见限制：

```text
文件描述符数量
listen 队列大小
TCP backlog
内存
网卡
客户端压测能力
```

Windows 常见限制：

```text
必须使用 64 位程序
不能使用 select
需要 IOCP
需要足够内存
需要调整动态端口范围
压测通常需要多台客户端机器
```

所以准确说：

```text
本项目选择的是支持百万连接的正确架构方向
真正达到百万连接还需要后续压测和系统调优
```

## 当前版本还有哪些不足

当前版本是第一阶段高并发骨架，不是最终工业级服务器。

还需要继续补：

```text
Linux/macOS 多线程 Reactor
Windows AcceptEx 异步 accept
连接超时回收
异步日志
HTTP 协议解析
压测工具
百万连接调优脚本
更完善的错误处理
```

Linux/macOS 当前已经有多线程 Reactor 第一版：

```text
主线程负责 accept
多个 worker EventLoop 负责连接读写
每个连接固定绑定一个 worker
```

后续还需要继续补：

```text
EventLoop 跨线程唤醒机制
连接定时器
更完整的多线程压测
```

## 一句话总结

这个服务器实现高并发的原理是：

```text
用少量线程管理大量连接
用非阻塞 IO 避免线程卡死
用 epoll/kqueue/IOCP 让操作系统通知活跃连接
用 Buffer 处理网络读写不完整的问题
用事件循环把 accept/read/write 串起来
```

所以它不是靠“线程多”实现高并发，而是靠“线程少、事件驱动、只处理活跃连接”实现高并发。
