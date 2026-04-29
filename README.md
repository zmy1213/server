# C++20 Server

一个从零开始实现的 C++20 跨平台 TCP 服务器项目。

项目目标是先搭建一个清晰、可学习、可扩展的服务器骨架，再逐步升级到高并发服务器。当前版本已经支持 CMake 构建、跨平台 socket 封装、非阻塞 IO、事件循环和 Echo Server 示例。

仓库地址：

```text
https://github.com/zmy1213/server
```

## 当前功能

- 使用 C++20 编写
- 使用 CMake 构建
- 支持 macOS、Linux、Windows
- 支持非阻塞 TCP socket
- 支持 Reactor 事件循环模型
- Linux 自动使用 `epoll`
- macOS 自动使用 `kqueue`
- Windows 自动使用 `select`
- 提供 Echo Server 示例

说明：Windows 当前使用 `select` 是为了保证兼容和方便学习。如果后续要在 Windows 上实现真正的百万级连接，需要增加 `IOCP` 后端。

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
├── examples/
│   └── echo_server.cpp
├── include/
│   └── cpp20_server/
│       └── net/
│           ├── buffer.h
│           ├── poller.h
│           ├── socket.h
│           └── tcp_server.h
└── src/
    └── net/
        ├── buffer.cpp
        ├── poller.cpp
        ├── socket.cpp
        └── tcp_server.cpp
```

核心模块说明：

```text
buffer      输出缓冲区
socket      跨平台 socket 封装
poller      epoll/kqueue/select 的统一事件接口
tcp_server  TCP 服务器主逻辑
examples    示例程序
```

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
Windows  -> select
```

也可以手动指定：

```bash
cmake -S . -B build -DCPP20_SERVER_BACKEND=epoll
cmake -S . -B build -DCPP20_SERVER_BACKEND=kqueue
cmake -S . -B build -DCPP20_SERVER_BACKEND=select
```

注意：

```text
epoll  只能在 Linux 使用
kqueue 当前只在 macOS 开启
select 兼容性最好，但不适合百万连接
```

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
8. 增加 Windows IOCP 后端

## 当前阶段说明

当前版本是学习和扩展用的第一版服务器骨架，重点是：

```text
先跑通跨平台构建
再跑通非阻塞 TCP 服务
再逐步升级到高并发架构
```

不要一开始就把所有功能写复杂。服务器开发的正确顺序是先保证能运行、能测试、结构清晰，然后再逐步加多线程、定时器、协议解析和性能优化。
