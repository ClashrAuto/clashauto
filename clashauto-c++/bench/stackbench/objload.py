#!/usr/bin/env python3
"""小对象负载 —— 模拟真实网关流量（大量短连接 + 小响应），而非单条 bulk 流。
存在理由：R5 发现 smoltcp 的一大截优势来自 GRO 批量合并，而 GRO 只在长流上生效。
真实网页流量是「连上→拿一个几十 KB 的对象→关闭」，每条流都太短，GRO 摊不开。
这个负载就是要看：单流 4× 的优势，在小对象上还剩多少。

server: 连上就发 OBJ 字节然后关闭。
client: WORKERS 个并发 worker，每个循环 connect→读完→close，跑 DUR 秒，报 完成数/字节数。
"""
import os
import socket
import sys
import threading
import time

OBJ = int(os.environ.get("OBJ", 64 * 1024))   # 每个对象大小，默认 64 KiB


def serve(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(128)
    payload = b"x" * OBJ
    while True:
        c, _ = s.accept()
        try:
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            c.recv(64)            # 读掉客户端的一个字节请求
            c.sendall(payload)
        except OSError:
            pass
        finally:
            c.close()


def client(host, port, workers, dur):
    stop = time.time() + dur
    counts = [0] * workers
    bytes_ = [0] * workers
    errs = [0] * workers

    def work(i):
        while time.time() < stop:
            try:
                c = socket.create_connection((host, port), timeout=4)
                c.sendall(b"g")
                got = 0
                while True:
                    b = c.recv(65536)
                    if not b:
                        break
                    got += len(b)
                c.close()
                counts[i] += 1
                bytes_[i] += got
            except OSError:
                errs[i] += 1

    ths = [threading.Thread(target=work, args=(i,)) for i in range(workers)]
    t0 = time.time()
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    dt = time.time() - t0
    tot, tb, te = sum(counts), sum(bytes_), sum(errs)
    print('{"objs":%d,"bytes":%d,"err":%d,"sec":%.2f,"obj_per_s":%.0f,"gbps":%.3f,"obj_bytes":%d}'
          % (tot, tb, te, dt, tot / dt, tb * 8 / dt / 1e9, OBJ))


if __name__ == "__main__":
    if sys.argv[1] == "server":
        serve(int(sys.argv[2]))
    else:
        client(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]))
