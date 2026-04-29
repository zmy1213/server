#!/usr/bin/env python3
"""Small HTTP benchmark tool for the teaching C++20 server.

The script intentionally uses only Python's standard library so it works on
macOS, Linux, and Windows without installing wrk/ab/hey first.
"""

from __future__ import annotations

import argparse
import asyncio
import collections
import dataclasses
import statistics
import time
from typing import List, Optional, Tuple


class ProtocolError(RuntimeError):
    pass


@dataclasses.dataclass
class SharedState:
    total_requests: int
    issued_requests: int = 0

    def take_request(self) -> bool:
        if self.issued_requests >= self.total_requests:
            return False
        self.issued_requests += 1
        return True


@dataclasses.dataclass
class BenchStats:
    successful_requests: int = 0
    failed_requests: int = 0
    bytes_read: int = 0
    bytes_written: int = 0
    opened_connections: int = 0
    latencies_ms: List[float] = dataclasses.field(default_factory=list)
    errors: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    status_codes: collections.Counter = dataclasses.field(default_factory=collections.Counter)

    def merge(self, other: "BenchStats") -> None:
        self.successful_requests += other.successful_requests
        self.failed_requests += other.failed_requests
        self.bytes_read += other.bytes_read
        self.bytes_written += other.bytes_written
        self.opened_connections += other.opened_connections
        self.latencies_ms.extend(other.latencies_ms)
        self.errors.update(other.errors)
        self.status_codes.update(other.status_codes)


def build_request(args: argparse.Namespace) -> bytes:
    body = args.body.encode("utf-8")
    headers = [
        f"{args.method} {args.path} HTTP/1.1",
        f"Host: {args.host}:{args.port}",
        "User-Agent: cpp20-server-bench/1.0",
        "Accept: */*",
        "Connection: keep-alive",
        f"Content-Length: {len(body)}",
        "",
        "",
    ]
    return "\r\n".join(headers).encode("ascii") + body


async def open_connection(args: argparse.Namespace) -> Tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    return await asyncio.wait_for(
        asyncio.open_connection(args.host, args.port),
        timeout=args.timeout,
    )


async def read_http_response(reader: asyncio.StreamReader, timeout: float) -> Tuple[int, int]:
    header_bytes = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=timeout)
    header_text = header_bytes.decode("iso-8859-1")
    lines = header_text.split("\r\n")
    if not lines or not lines[0].startswith("HTTP/"):
        raise ProtocolError("missing HTTP status line")

    status_parts = lines[0].split(" ", 2)
    if len(status_parts) < 2 or not status_parts[1].isdigit():
        raise ProtocolError("invalid HTTP status line")
    status_code = int(status_parts[1])

    content_length = 0
    for line in lines[1:]:
        name, separator, value = line.partition(":")
        if separator and name.strip().lower() == "content-length":
            try:
                content_length = int(value.strip())
            except ValueError as exc:
                raise ProtocolError("invalid Content-Length") from exc

    body_size = 0
    if content_length > 0:
        body = await asyncio.wait_for(reader.readexactly(content_length), timeout=timeout)
        body_size = len(body)

    return status_code, len(header_bytes) + body_size


def close_writer(writer: Optional[asyncio.StreamWriter]) -> None:
    if writer is not None:
        writer.close()


async def wait_writer_closed(writer: Optional[asyncio.StreamWriter]) -> None:
    if writer is None:
        return
    try:
        await writer.wait_closed()
    except (ConnectionError, OSError):
        pass


async def worker(worker_id: int,
                 args: argparse.Namespace,
                 request_bytes: bytes,
                 state: SharedState) -> BenchStats:
    del worker_id
    stats = BenchStats()
    reader: Optional[asyncio.StreamReader] = None
    writer: Optional[asyncio.StreamWriter] = None

    while state.take_request():
        if writer is None or writer.is_closing():
            try:
                reader, writer = await open_connection(args)
                stats.opened_connections += 1
            except (TimeoutError, OSError) as exc:
                stats.failed_requests += 1
                stats.errors[type(exc).__name__] += 1
                reader = None
                writer = None
                continue

        started = time.perf_counter()
        try:
            writer.write(request_bytes)
            await asyncio.wait_for(writer.drain(), timeout=args.timeout)
            status_code, bytes_read = await read_http_response(reader, args.timeout)
            elapsed_ms = (time.perf_counter() - started) * 1000.0

            stats.bytes_written += len(request_bytes)
            stats.bytes_read += bytes_read
            stats.status_codes[status_code] += 1
            stats.latencies_ms.append(elapsed_ms)
            if status_code == args.expect_status:
                stats.successful_requests += 1
            else:
                stats.failed_requests += 1
                stats.errors[f"unexpected_status_{status_code}"] += 1
        except (asyncio.IncompleteReadError, TimeoutError, ConnectionError, OSError, ProtocolError) as exc:
            stats.failed_requests += 1
            stats.errors[type(exc).__name__] += 1
            close_writer(writer)
            await wait_writer_closed(writer)
            reader = None
            writer = None

    close_writer(writer)
    await wait_writer_closed(writer)
    return stats


def percentile(sorted_values: List[float], percent: float) -> float:
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    rank = (len(sorted_values) - 1) * percent
    lower = int(rank)
    upper = min(lower + 1, len(sorted_values) - 1)
    weight = rank - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def format_counter(counter: collections.Counter) -> str:
    if not counter:
        return "none"
    return ", ".join(f"{key}={value}" for key, value in sorted(counter.items(), key=lambda item: str(item[0])))


async def run_benchmark(args: argparse.Namespace) -> Tuple[BenchStats, float]:
    request_bytes = build_request(args)
    state = SharedState(total_requests=args.requests)
    workers = [
        asyncio.create_task(worker(i, args, request_bytes, state))
        for i in range(args.connections)
    ]

    started = time.perf_counter()
    partial_results = await asyncio.gather(*workers)
    elapsed = time.perf_counter() - started

    merged = BenchStats()
    for result in partial_results:
        merged.merge(result)
    return merged, elapsed


def print_report(args: argparse.Namespace, stats: BenchStats, elapsed_seconds: float) -> None:
    total_completed = stats.successful_requests + stats.failed_requests
    qps = stats.successful_requests / elapsed_seconds if elapsed_seconds > 0 else 0.0
    error_rate = (stats.failed_requests / total_completed * 100.0) if total_completed else 0.0
    latencies = sorted(stats.latencies_ms)
    avg_latency = statistics.fmean(latencies) if latencies else 0.0

    print("HTTP benchmark result")
    print(f"target: http://{args.host}:{args.port}{args.path}")
    print(f"method: {args.method}")
    print(f"configured_concurrency: {args.connections}")
    print(f"opened_connections: {stats.opened_connections}")
    print(f"requested_requests: {args.requests}")
    print(f"completed_requests: {total_completed}")
    print(f"successful_requests: {stats.successful_requests}")
    print(f"failed_requests: {stats.failed_requests}")
    print(f"error_rate_percent: {error_rate:.2f}")
    print(f"elapsed_seconds: {elapsed_seconds:.3f}")
    print(f"qps_success: {qps:.2f}")
    print(f"bytes_written: {stats.bytes_written}")
    print(f"bytes_read: {stats.bytes_read}")
    print("latency_ms:")
    print(f"  min: {latencies[0] if latencies else 0.0:.3f}")
    print(f"  avg: {avg_latency:.3f}")
    print(f"  p50: {percentile(latencies, 0.50):.3f}")
    print(f"  p90: {percentile(latencies, 0.90):.3f}")
    print(f"  p95: {percentile(latencies, 0.95):.3f}")
    print(f"  p99: {percentile(latencies, 0.99):.3f}")
    print(f"  max: {latencies[-1] if latencies else 0.0:.3f}")
    print(f"status_codes: {format_counter(stats.status_codes)}")
    print(f"errors: {format_counter(stats.errors)}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark a keep-alive HTTP endpoint.")
    parser.add_argument("--host", default="127.0.0.1", help="server host")
    parser.add_argument("--port", type=int, default=8080, help="server port")
    parser.add_argument("--path", default="/health", help="HTTP path")
    parser.add_argument("--method", default="GET", help="HTTP method")
    parser.add_argument("--body", default="", help="HTTP request body")
    parser.add_argument("-c", "--connections", type=int, default=100, help="concurrent TCP connections")
    parser.add_argument("-n", "--requests", type=int, default=10000, help="total HTTP requests")
    parser.add_argument("--timeout", type=float, default=5.0, help="connect/read/write timeout in seconds")
    parser.add_argument("--expect-status", type=int, default=200, help="expected HTTP status code")
    parser.add_argument("--fail-on-error", action="store_true", help="exit non-zero when any request fails")
    args = parser.parse_args()

    if args.connections <= 0:
        parser.error("--connections must be greater than 0")
    if args.requests <= 0:
        parser.error("--requests must be greater than 0")
    if args.port <= 0 or args.port > 65535:
        parser.error("--port must be between 1 and 65535")
    args.method = args.method.upper()
    if not args.path.startswith("/"):
        args.path = "/" + args.path
    return args


def main() -> int:
    args = parse_args()
    stats, elapsed = asyncio.run(run_benchmark(args))
    print_report(args, stats, elapsed)
    if args.fail_on_error and stats.failed_requests > 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
