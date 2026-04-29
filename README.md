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
```

## 当前功能

- 使用 C++20 编写
- 使用 CMake 构建
- 支持 macOS、Linux、Windows
- 支持非阻塞 TCP socket
- 支持 Reactor 事件循环模型
- Linux 自动使用 `epoll`
- macOS 自动使用 `kqueue`
- Windows 自动使用 `IOCP`
- 提供 Echo Server 示例

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
│   ├── HIGH_CONCURRENCY.md
│   └── LEARNING_ROADMAP.md
├── examples/
│   └── echo_server.cpp
├── include/
│   └── cpp20_server/
│       └── net/
│           ├── acceptor.h
│           ├── buffer.h
│           ├── channel.h
│           ├── connection.h
│           ├── event_loop.h
│           ├── poller.h
│           ├── socket.h
│           └── tcp_server.h
└── src/
    └── net/
        ├── acceptor.cpp
        ├── buffer.cpp
        ├── channel.cpp
        ├── connection.cpp
        ├── event_loop.cpp
        ├── poller.cpp
        ├── socket.cpp
        └── tcp_server.cpp
```

核心模块说明：

```text
acceptor     监听 socket，专门负责 accept 新连接
buffer       输出缓冲区
channel      一个 fd 关心的事件和回调
connection   单个客户端连接的读写状态
event_loop   Reactor 事件循环
poller       epoll/kqueue/select 的统一事件接口
socket       跨平台 socket 封装
tcp_server   TCP 服务器入口，Windows IOCP 也在这里实现
examples     示例程序
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

如果使用单配置生成器，比如 MinGW，也可能是：

```powershell
.\build\echo_server.exe 0.0.0.0 8080
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

1. 增加多线程 Reactor
2. 增加连接分发到 worker 线程
3. 增加定时器和空闲连接回收
4. 增加 HTTP 协议解析
5. 增加异步日志
6. 增加压测脚本
7. 增加 Linux 百万连接参数调优文档
8. 增加 Windows AcceptEx 批量异步接收连接

## 当前阶段说明

当前版本是学习和扩展用的第一版服务器骨架，重点是：

```text
先跑通跨平台构建
再跑通非阻塞 TCP 服务
再逐步升级到高并发架构
```

不要一开始就把所有功能写复杂。服务器开发的正确顺序是先保证能运行、能测试、结构清晰，然后再逐步加多线程、定时器、协议解析和性能优化。
