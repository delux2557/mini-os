#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Host-side deterministic test for upstream reliable stop-and-wait of tcp_proxy.py.

Fake "guest" client:
  - sends MSG_OPEN to open a session to a real upstream TCP server
  - sends N blocks as guest->host MSG_DATA with increasing seq, waiting for the
    host->guest MSG_ACK (next seq) after each; injects ONE duplicate block to prove
    the proxy drops it (idempotent) and never double-delivers upstream.

Assertions:
  1. every ordered block is ACKed with next == seq+1
  2. a duplicate seq is NOT re-forwarded upstream (dedup) and the ACK is still sent
  3. the upstream TCP server receives all data exactly once, in order

Usage: python3 tests/test_upstream_reliable.py [proxy-port] [upstream-port]
Exit 0 = pass / 1 = fail. Requires tcp_proxy.py to already be running in --mode udp.
"""
import socket, sys, threading, time, struct

PROXY = ('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 7778)
UP    = ('127.0.0.1', int(sys.argv[2]) if len(sys.argv) > 2 else 8081)
SID   = 0xdeadbeef

MSG_DATA, MSG_OPENED, MSG_CLOSED, MSG_ERROR, MSG_TIMEOUT, MSG_OPEN, MSG_CLOSE, MSG_ACK = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08)
HDR = 8

BLOCKS = [
    b'block 0: ' + b'X' * 1200,
    b'block 1: ' + b'Y' * 1200,
    b'block 2: ' + b'Z' * 1185,
]
EXPECTED = b''.join(BLOCKS)

lock = threading.Lock()
got = b''; ok = False

def hdr(mtype, seq=0):
    return struct.pack('>IBBH', SID, mtype, 1, seq)   # sid + type + ver1 + seq

def upstream_server():
    global got, ok
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(UP); s.listen(1)
    conn, _ = s.accept()
    while True:
        b = conn.recv(4096)
        if not b: break
        with lock:
            got += b
            ok = (got == EXPECTED)          # 即时判定：数据齐即标记 ok（不依赖 TCP 关闭）
    conn.close(); s.close()
    print(f'[upstream] received {len(got)} bytes, ok={ok}', flush=True)

def recv_ack(sock, want, tmo=3.0):
    """Wait until we receive MSG_ACK whose payload next == want. Return True/False."""
    end = time.time() + tmo
    while time.time() < end:
        try:
            buf, _ = sock.recvfrom(64)
            if len(buf) < HDR + 2: continue
            if buf[4] == MSG_ACK:
                nxt = (buf[HDR] << 8) | buf[HDR + 1]
                if nxt == want: return True
        except socket.timeout:
            break
    return False

def wait_opened(sock, tmo=3.0):
    """Drain until we see MSG_OPENED (session truly open). Return True/False."""
    end = time.time() + tmo
    while time.time() < end:
        try:
            buf, _ = sock.recvfrom(64)
            if buf and buf[4] == MSG_OPENED: return True
        except socket.timeout:
            break
    return False

def main():
    athr = threading.Thread(target=upstream_server, daemon=True); athr.start()
    time.sleep(0.2)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.settimeout(0.5)
    sock.bind(('127.0.0.1', 0))          # pick our own UDP src port

    # MSG_OPEN: payload = dst_ip(4BE) + dst_port(2BE)
    sock.sendto(hdr(MSG_OPEN) + struct.pack('>IH', 0x7f000001, UP[1]), PROXY)
    if not wait_opened(sock):
        print('[FAIL] session never opened'); sys.exit(1)
    print('[ok] session OPENED')

    for seq, block in enumerate(BLOCKS):
        sock.sendto(hdr(MSG_DATA, seq) + block, PROXY)
        if not recv_ack(sock, seq + 1):
            print(f'[FAIL] no ACK(next={seq+1}) for block seq={seq}'); sys.exit(1)
        print(f'[ok] block seq={seq} -> ACK {seq+1}')
        if seq == 1:
            # duplicate: same seq re-sent -> proxy must drop (not forward again) but still ACK
            sock.sendto(hdr(MSG_DATA, 1) + BLOCKS[1], PROXY)
            if not recv_ack(sock, 2):
                print('[FAIL] dup seq=1: expected ACK 2 (still there)'); sys.exit(1)
            print('[ok] duplicate seq=1 dropped; ACK 2 still delivered')

    # 轮询等待上游数据齐全（即时判定，不需 TCP 关闭）
    endt = time.time() + 3.0; done = False
    while time.time() < endt:
        with lock:
            if ok: done = True; break
        time.sleep(0.1)
    with lock:
        if not done:
            print(f'[FAIL] upstream timeout: expected {len(EXPECTED)}B got {len(got)}B')
            for i, b in enumerate(BLOCKS):
                print(f'  block {i}: len={len(b)} appears_in_got={got.count(b)}')
            sys.exit(1)
    print('[PASS] upstream reliable OK: ordered, deduped, ACKed, received exactly once')
    sys.exit(0)

if __name__ == '__main__':
    main()