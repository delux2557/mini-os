#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Host-side deterministic test for the upstream **sliding-window** receive of tcp_proxy.py.

Contrast with test_upstream_reliable.py (which exercises the fragment-level stop-and-wait
handshake). Here we verify the window receiver where the sender may have MANY packets in
flight at once and may deliver them OUT OF ORDER:

  1. OPEN a session to a real upstream TCP server.
  2. Burst a batch of guest->host MSG_DATA in order (= WIN packets) WITHOUT waiting for ACK
     each time -> proxy must forward all to TCP and issue cumulative ACK (next=WIN).
  3. Inject an OUT-OF-ORDER block: send seq=N+1 before seq=N -> proxy must BUFFER it, ACK
     stays at N; then send seq=N -> proxy drains the gap, forwards both in order, ACK jumps
     to N+2 (cumulative).
  4. Inject a DUPLICATE seq -> must NOT be re-forwarded (no double delivery).

Assertions:
  - every cumulative ACK eventually reflects highest contiguous seq + 1
  - the upstream TCP server receives ALL data exactly once, in order
  - out-of-order buffering is observed (ACK holds, then jumps) and resolves

Usage: python3 tests/test_upstream_window.py [proxy-port] [upstream-port]
Exit 0 = pass / 1 = fail. Requires tcp_proxy.py to already be running in --mode udp.
"""
import socket, sys, threading, time, struct

PROXY = ('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 7778)
UP    = ('127.0.0.1', int(sys.argv[2]) if len(sys.argv) > 2 else 8082)
SID   = 0xfeedc0de
W     = 8            # 与客/代理 TCP_TXWIN / UP_WIN 对齐

MSG_DATA, MSG_OPENED, MSG_CLOSED, MSG_ERROR, MSG_TIMEOUT, MSG_OPEN, MSG_CLOSE, MSG_ACK = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08)
HDR = 8

# 一批在序块（W 个），随后一组乱序对
BATCH  = [('batch', i, b'B%d:' % i + b'M' * 1200) for i in range(W)]
OOO_LO = ('ooo-low',  W,     b'ooo-low: '  + b'L' * 100)
OOO_HI = ('ooo-high', W + 1, b'ooo-high: ' + b'H' * 100)
DUP    = ('dup',      W + 1, OOO_HI[2])       # 重复 OOO_HI 的 seq
ALL     = BATCH + [OOO_LO, OOO_HI]
EXPECTED = b''.join(b for _, _, b in ALL)

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
            ok = (got == EXPECTED)
    conn.close(); s.close()
    print(f'[upstream] received {len(got)} bytes, ok={ok}', flush=True)

def drain_acks(sock, tmo=3.0):
    """Drain socket collecting all MSG_ACK 'next' values; return list."""
    out, end = [], time.time() + tmo
    while time.time() < end:
        try:
            sock.settimeout(0.25)
            buf, _ = sock.recvfrom(64)
            if len(buf) >= HDR + 2 and buf[4] == MSG_ACK:
                out.append((buf[HDR] << 8) | buf[HDR + 1])
        except socket.timeout:
            break
    sock.settimeout(0.5)
    return out

def wait_opened(sock, tmo=3.0):
    end = time.time() + tmo
    while time.time() < end:
        try:
            sock.settimeout(0.5)
            buf, _ = sock.recvfrom(64)
            if buf and buf[4] == MSG_OPENED: return True
        except socket.timeout:
            break
    return False

def main():
    thr = threading.Thread(target=upstream_server, daemon=True); thr.start()
    time.sleep(0.2)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.settimeout(0.5)
    sock.bind(('127.0.0.1', 0))

    sock.sendto(hdr(MSG_OPEN) + struct.pack('>IH', 0x7f000001, UP[1]), PROXY)
    if not wait_opened(sock):
        print('[FAIL] session never opened'); sys.exit(1)
    print('[ok] session OPENED')

    # ---- 2) 在序流水线 burst：一次全发，不逐块等 ACK ----
    for _, seq, blk in BATCH:
        sock.sendto(hdr(MSG_DATA, seq) + blk, PROXY)
    acks = drain_acks(sock)
    if W not in acks:
        print(f'[FAIL] burst 后未收到累计 ACK {W} (got acks={acks})'); sys.exit(1)
    print(f'[ok] 在序 W={W} 齐发 -> 累计 ACK 推进到 {W} (acks={acks})')

    # ---- 3) 乱序缓冲：先发 seq=W+1（gap），ACK 应停在 W；再发 seq=W 凑齐 -> 累计跳到 W+2 ----
    sock.sendto(hdr(MSG_DATA, OOO_HI[1]) + OOO_HI[2], PROXY)   # 先高 seq
    pre = drain_acks(sock, 1.0)
    if any(a > W for a in pre):
        print(f'[FAIL] 乱序高层不应推进 ACK 到 {max(pre)} (got {pre})'); sys.exit(1)
    print(f'[ok] 乱序 seq={OOO_HI[1]} 被暂存，累计 ACK 仍保持 {W} (acks={pre})')

    sock.sendto(hdr(MSG_DATA, OOO_LO[1]) + OOO_LO[2], PROXY) # 补低 seq 凑齐
    post = drain_acks(sock, 1.0)
    if (W + 2) not in post:
        print(f'[FAIL] 补位后未累计推进到 {W+2} (got {post})'); sys.exit(1)
    print(f'[ok] 补 seq={OOO_LO[1]} -> 连续排空，累计 ACK 跳到 {W+2} (acks={post})')

    # ---- 4) 重复 seq：不得转发第二遍，ACK 保持，上游总量不变 ----
    sock.sendto(hdr(MSG_DATA, DUP[1]) + DUP[2], PROXY)
    dup_acks = drain_acks(sock, 1.0)
    if dup_acks and max(dup_acks) > W + 2:
        print(f'[FAIL] 重复 seq 竟推进了 ACK ({dup_acks})'); sys.exit(1)
    print(f'[ok] 重复 seq={DUP[1]} 未推进 ACK (acks={dup_acks})')

    # ---- 轮询等上游数据齐全，且确认总量 == EXPECTED（每块恰好一次、按序） ----
    endt = time.time() + 3.0; done = False
    while time.time() < endt:
        with lock:
            if ok: done = True; break
        time.sleep(0.1)
    with lock:
        if not done:
            print(f'[FAIL] upstream timeout/order: expected {len(EXPECTED)}B got {len(got)}B')
            for _, seq, blk in ALL:
                print(f'  seq={seq} len={len(blk)} appears={got.count(blk)}')
            sys.exit(1)
    print('[PASS] upstream sliding window OK: pipelined burst + out-of-order buffering + dedup, '
          'received exactly once in order')
    sys.exit(0)

if __name__ == '__main__':
    main()