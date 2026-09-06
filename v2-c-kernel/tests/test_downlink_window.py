#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Host-side deterministic test for the proxy's **downlink sliding-window** sender.

Contrast:
  - test_upstream_window.py : tests the proxy's *upstream receiver* window (v1.3).
  - test_tcp_dl.sh          : end-to-end 128KB *download* through QEMU (acceptance).
Here we verify the proxy's DOWNSTREAM *sender* window (v1.4) without QEMU:
  - the proxy keeps at most DWIN downlink datagrams in flight (no unbounded burst);
  - it does NOT advance until the guest ACKs cumulatively (下一期望下行 seq);
  - on ACK timeout it retransmits the OLDEST unacked downlink slot (dup seen);
  - it only sends MSG_CLOSED after every downlink byte is acked (no missing tail);
  - every payload byte is delivered exactly once, in order over the seq domain.

自包含：本测试自己拉起 `tcp_proxy.py --mode udp` 与一个上游 TCP 服务端，跑完即清理，
可独立进 CI。用法: python3 tests/test_downlink_window.py [proxy-port] [upstream-port]
Exit 0 = pass / 1 = fail.
"""
import socket, sys, threading, time, subprocess, os, struct

PROXY_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7783
UP_PORT    = int(sys.argv[2]) if len(sys.argv) > 2 else 8089
SID   = 0x0feed1001
K     = 1380            # 每块净载荷（刻意 ≠ CHUNK=1392，验证代理按字节切报而非按块）
NPKTS = 12              # > DWIN → 必跨多轮窗口滑动
DWIN  = 8

MSG_DATA, MSG_OPENED, MSG_CLOSED, MSG_ERROR, MSG_TIMEOUT, MSG_OPEN, MSG_CLOSE, MSG_ACK = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08)
HDR = 8

BLOB = (b'X' * K + b'Y' * 3) * NPKTS     # 总长非 CHUNK 整数倍，验证尾部切分正确

def hdr(mtype, seq=0):
    return struct.pack('>IBBH', SID, mtype, 1, seq)

def read_pkt(sock, tmo):
    try:
        sock.settimeout(tmo)
        buf, _ = sock.recvfrom(8192)
        if len(buf) < HDR: return None
        return (buf[4], (buf[6] << 8) | buf[7], buf[HDR:])   # (type, seq, payload)
    except socket.timeout:
        return None

def main():
    proxy = subprocess.Popen(
        [sys.executable, 'tests/tcp_proxy.py', '--mode', 'udp',
         '--port', str(PROXY_PORT), '--timeout', '8'],
        cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    time.sleep(0.3)
    try:
        _run(proxy)
    finally:
        proxy.terminate(); proxy.wait()

def _run(proxy):
    # 上游服务端：连接后一次性下发整个大块并关闭（驱动代理 _tcp_read 分片读走）
    def upstream_server():
        s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.settimeout(6)
        s.bind(('127.0.0.1', UP_PORT)); s.listen(1)
        try:
            try:
                conn, _ = s.accept()
            except OSError:
                s.close(); return
            conn.settimeout(6)
            conn.sendall(BLOB)
            conn.close(); s.close()
        except OSError:
            pass
    threading.Thread(target=upstream_server, daemon=True).start()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.bind(('127.0.0.1', 0))
    sock.sendto(hdr(MSG_OPEN) + struct.pack('>IH', 0x7f000001, UP_PORT), ('127.0.0.1', PROXY_PORT))

    # 等 OPENED
    t0 = time.time(); opened = False
    while time.time() - t0 < 3:
        p = read_pkt(sock, 0.3)
        if p and p[0] == MSG_OPENED: opened = True; break
    if not opened:
        print(f'[FAIL] session never opened (proxy alive={proxy.poll() is None})'); sys.exit(1)
    print('[ok] session OPENED')

    # ---- 断言 1：首轮窗口填满 DWIN=seq 0..7 后在序停止（不再爆出第 9 个）----
    wave = {}
    t0 = time.time()
    while time.time() - t0 < 2.0 and len(wave) < DWIN:
        p = read_pkt(sock, 0.3) or (None, None, None)
        if p[0] == MSG_DATA and p[1] not in wave: wave[p[1]] = p[2]
    if sorted(wave) != list(range(DWIN)):
        print(f'[FAIL] 首轮应为在序 seq 0..{DWIN-1}，got {sorted(wave)}'); sys.exit(1)
    print(f'[ok] 首轮在序填满 DWIN={DWIN}: seq 0..{DWIN-1}')
    extra = read_pkt(sock, 0.6)          # 窗口满，不得再发
    if extra and extra[0] == MSG_DATA:
        print(f'[FAIL] 窗口满仍多发 downlink seq={extra[1]}'); sys.exit(1)
    print('[ok] 满窗不再爆（≤ DWIN 在途）')

    # ---- 断言 2：不发 ACK → ~RETX_MS 后重传最老未确认槽（seq0 重复出现）----
    dup = None
    t0 = time.time()
    while time.time() - t0 < 3.5:
        p = read_pkt(sock, 0.3) or (None, None, None)
        if p[0] == MSG_DATA and p[1] == 0: dup = p[1]; break
    if dup is None:
        print('[FAIL] 未观察到最老未确认槽超时重传（seq0 应 ~2s 后重发）'); sys.exit(1)
    print('[ok] 最老未确认槽超时重传（seq0 重复）')

    # ---- 断言 3：累计 ACK 逐窗推进，直到 CLOSED；按序不重不漏 == BLOB ----
    got = dict(wave)                      # {seq: payload}，重复 seq 忽略
    acked = 0                             # 已累计确认到的连续计数
    done = False; clock = time.time()
    def contig_top():
        n = 0
        while n in got: n += 1
        return n
    while not done and time.time() - clock < 15:
        n = contig_top()
        if n > acked:
            sock.sendto(hdr(MSG_ACK) + struct.pack('>H', n), ('127.0.0.1', PROXY_PORT))
            acked = n
        p = read_pkt(sock, 0.15)
        if not p: continue
        if p[0] == MSG_DATA:
            if p[1] in got: pass            # 重传重复，忽略
            else: got[p[1]] = p[2]
        elif p[0] == MSG_CLOSED:
            done = True
    if not done:
        print(f'[FAIL] 未收到 MSG_CLOSED（数据仍未齐？got seq = {sorted(got)}）'); sys.exit(1)

    rebuild = b''.join(got[i] for i in sorted(got))
    if rebuild != BLOB:
        print(f'[FAIL] 下行重组与上游 BLOB 不一致：got {len(rebuild)}B, want {len(BLOB)}B')
        sys.exit(1)
    if len(got) < NPKTS:
        print(f'[FAIL] 数据报分片数异常：{len(got)} (期望 ≥ {NPKTS})'); sys.exit(1)
    print(f'[ok] 全量下行重组一致：{len(rebuild)}B，分片 {len(got)} 报，无重、按序')
    print('[PASS] downlink sliding-window sender OK: window cap + cumulative ACK + '
          'oldest-slot retransmit + no missing tail')
    sys.exit(0)

if __name__ == '__main__':
    main()