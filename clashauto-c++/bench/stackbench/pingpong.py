#!/usr/bin/env python3
"""延迟测量：不是 ping，而是**走被测栈的 TCP 路径**做小包乒乓。

server 模式：127.0.0.1:5202 上原样回显（被测栈会把连接直连到这个端口）。
client 模式：经隧道地址连过去，做 N 次「发 1 字节 → 等 1 字节」串行往返，
             报告建连时间与往返 p50/p90/p99。串行是故意的——要量的是单次往返延迟，
             并发会把排队时间混进来。
"""
import json
import socket
import statistics
import sys
import time


def serve(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(64)
    while True:
        c, _ = s.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            while True:
                b = c.recv(4096)
                if not b:
                    break
                c.sendall(b)
        except OSError:
            pass
        finally:
            c.close()


def client(host, port, n):
    t0 = time.perf_counter_ns()
    c = socket.create_connection((host, port), timeout=5)
    connect_ms = (time.perf_counter_ns() - t0) / 1e6
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    rtts = []
    payload = b"x"
    for _ in range(n):
        a = time.perf_counter_ns()
        c.sendall(payload)
        got = c.recv(16)
        if not got:
            break
        rtts.append((time.perf_counter_ns() - a) / 1e6)
    c.close()
    rtts.sort()

    def pct(p):
        if not rtts:
            return 0.0
        k = min(len(rtts) - 1, int(round(p / 100.0 * (len(rtts) - 1))))
        return round(rtts[k], 4)

    print(json.dumps({
        "connect_ms": round(connect_ms, 4),
        "n": len(rtts),
        "p50_ms": pct(50), "p90_ms": pct(90), "p99_ms": pct(99),
        "max_ms": round(rtts[-1], 4) if rtts else 0.0,
        "mean_ms": round(statistics.fmean(rtts), 4) if rtts else 0.0,
    }))


if __name__ == "__main__":
    if sys.argv[1] == "server":
        serve(int(sys.argv[2]))
    else:
        client(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
