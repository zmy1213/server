# 压测脚本说明

这个项目现在提供一个无第三方依赖的 HTTP 压测脚本：

```text
tools/bench_http.py
```

它用 Python 标准库 `asyncio` 实现。每个 asyncio worker 维护一个 TCP 连接，在连接上循环发送 HTTP 请求，并统计：

```text
QPS
并发连接数
成功请求数
失败请求数
错误类型
HTTP 状态码分布
延迟 min / avg / p50 / p90 / p95 / p99 / max
读写字节数
```

## 启动服务器

先构建项目：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

启动 HTTP Server：

```bash
./build/http_server --config examples/server.conf
```

如果 8080 端口被占用，可以临时换端口：

```bash
./build/http_server --config examples/server.conf 127.0.0.1 18080 4 30
```

## 运行压测

小规模压测：

```bash
python3 tools/bench_http.py --host 127.0.0.1 --port 18080 --path /health -c 50 -n 5000
```

参数含义：

```text
--host              服务器地址
--port              服务器端口
--path              HTTP 路径
-c, --connections   并发 TCP 连接数
-n, --requests      总请求数
--timeout           单次连接/读/写超时时间
--expect-status     期望的 HTTP 状态码，默认 200
--fail-on-error     只要有失败请求就返回非 0 退出码
```

## 输出怎么看

示例输出：

```text
HTTP benchmark result
target: http://127.0.0.1:18080/health
configured_concurrency: 50
requested_requests: 5000
successful_requests: 5000
failed_requests: 0
elapsed_seconds: 0.500
qps_success: 10000.00
latency_ms:
  p50: 1.200
  p90: 2.100
  p99: 6.500
errors: none
```

核心指标：

```text
qps_success          每秒成功请求数
configured_concurrency 本次压测设置的并发连接数
failed_requests      失败请求数量
error_rate_percent   失败请求比例
p50                  一半请求比这个延迟低
p90                  90% 请求比这个延迟低
p99                  99% 请求比这个延迟低，重点看尾延迟
```

小白理解：

```text
QPS 看吞吐量
错误数看稳定性
p50 看普通用户体验
p99 看最慢那批请求会不会拖垮系统
```

## 注意

本脚本适合项目当前阶段做功能型压测和趋势对比。真正百万连接压测还需要：

```text
Linux 系统参数调优
足够高的 ulimit -n
多台压测客户端机器
更专业的压测工具，比如 wrk、h2load、vegeta
服务端和客户端分开部署
```

单机 `127.0.0.1` 压测只能说明当前机器上的本地回环性能，不能代表真实公网或生产环境性能。

## 本机首轮结果

测试环境：

```text
macOS
backend=kqueue
server=./build/http_server
target=127.0.0.1:18080/health
worker_threads=4
```

命令：

```bash
python3 tools/bench_http.py --host 127.0.0.1 --port 18080 --path /health -c 50 -n 5000 --fail-on-error
```

结果：

```text
configured_concurrency: 50
opened_connections: 50
requested_requests: 5000
completed_requests: 5000
successful_requests: 5000
failed_requests: 0
error_rate_percent: 0.00
elapsed_seconds: 0.154
qps_success: 32409.77
bytes_written: 695000
bytes_read: 530000
latency_ms:
  min: 0.256
  avg: 1.500
  p50: 1.265
  p90: 1.577
  p95: 1.702
  p99: 2.412
  max: 22.472
status_codes: 200=5000
errors: none
```
