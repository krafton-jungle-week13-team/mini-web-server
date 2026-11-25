#!/usr/bin/env python3
"""프록시 테스트용 가짜 백엔드 (tests/test_proxy.sh가 띄워요)

    python3 tests/fake_backend.py <mode> <port>

mode
  echo   받은 요청 원문(요청줄+헤더+body)을 응답 body로 그대로 돌려줘요. 프록시가 무엇을 보냈는지 확인할 때 써요.
  slow   연결은 받지만 응답하지 않아요. 프록시가 기다리는 동안 다른 요청이 처리되는지 확인할 때 써요.
  empty  연결을 받자마자 응답 없이 닫아요. 502 확인용이에요.
"""
import socket
import sys
import threading

mode, port = sys.argv[1], int(sys.argv[2])
ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.1", port))
ls.listen(64)
held = []


def read_request(c):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = c.recv(65536)
        if not chunk:
            return data
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    length = 0
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        if k.strip().lower() == b"content-length":
            length = int(v.strip())
    while len(body) < length:
        chunk = c.recv(65536)
        if not chunk:
            break
        body += chunk
    return head + b"\r\n\r\n" + body


def handle(c):
    try:
        if mode == "empty":
            c.close()
            return
        if mode == "slow":
            held.append(c)          # 닫지 않고 붙잡아 둔다
            return
        raw = read_request(c)
        c.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                  b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(raw) + raw)
        c.close()
    except OSError:
        pass


while True:
    conn, _ = ls.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
