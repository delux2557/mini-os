#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# mini-os/v2-c-kernel/tests/tcp_attack.py
# 虚拟 TCP 压力/攻击注入器（与 test_tcp_attack.sh 配套，作为"脏输入并行注入"进程）。
# 方法：绑定宿主 UDP 作为攻击端，持续向 tcp_proxy.py --mode udp 监听端口 发送
# 脏会话数据报。tcp_proxy 按"源 (host,port)"区分会话，攻击源与 guest 真实来源不同，
# 因此注入脏数据不会破坏 guest→proxy 的真实连接，只会各自被独立路由；本脚本同时
# 读取任何可能的 proxy reply 并丢弃（不回 guest，不干扰真实链路）。
#
# 脏输入覆盖：未/错版本号 / 保留位非 0 / 非法 mtype / 过短头 / 随机字节 /
#           未知 sid 的 CLOSED/ERROR/TIMEOUT/OPENED/MSG_DATA / 方向非法 OPEN/CLOSE /
#           非 ASCII / MSG_DATA 大载 1392B 乱序 / 畸形头 0..7 字节
import socket, sys, time, struct, argparse, threading, random, os

random.seed(0xC500)
MSG_DATA, MSG_OPENED, MSG_CLOSED, MSG_ERROR, MSG_TIMEOUT, MSG_OPEN, MSG_CLOSE = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07)
PROTO_VERSION = 1

def hdr(sid, mtype, ver=PROTO_VERSION, flags=(0, 0)):
    return struct.pack('>I', sid) + bytes([mtype, ver, flags[0], flags[1]])

def build_templates():
    tpl = []
    # 1) 未知 sid 的下行事件——测试 guest conn_by_session(sid) 找不到 -> drop
    for mt in (MSG_CLOSED, MSG_ERROR, MSG_TIMEOUT, MSG_OPENED):
        for sid in (0, 0xFFFF, 0xFFFFFFFF, 12345, 42, 0xC0FFEE):
            tpl.append(hdr(sid, mt))
    # 2) 协议版本错 / 保留位非 0 / 非法 mtype
    for ver in (0, 2, 3, 0xFF):
        tpl.append(hdr(1, MSG_CLOSED, ver=ver))
    for fl in ((1, 2), (0xFF, 0xFF), (0xAA, 0x55)):
        tpl.append(hdr(1, MSG_CLOSED, flags=fl))
    for mt in list(range(0, 8)) + [0x0F, 0x10, 0x80, 0xFE, 0xFF]:
        tpl.append(hdr(77, mt))
    # 3) 未知 sid 的乱序 MSG_DATA（超大 / 全 0 / 非 ASCII 混）
    tpl.append(hdr(0xCAFE, MSG_DATA) + b'A' * 1392)
    tpl.append(hdr(0xCAFE, MSG_DATA) + b'\x00' * 7 + b'MSG' * 300)
    tpl.append(hdr(0x12345678, MSG_DATA) + (b'\xff\x00\x41' * 464))
    # 4) 过短头 / 随机字节
    for sz in range(0, 8):
        tpl.append(b'\x00' * sz)
    tpl.append(bytes(range(256)))
    return tpl

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--proxy', default='127.0.0.1:7778')
    ap.add_argument('--duration', type=int, default=15)
    ap.add_argument('--rate', type=int, default=200)   # 每线程每秒
    ap.add_argument('--threads', type=int, default=3)
    ap.add_argument('--wait', type=float, default=2.0,
                    help='proxy bind 尚在建立时重试此秒数，到目标接受第一包再开攻')
    args = ap.parse_args()

    host, port = args.proxy.rsplit(':', 1)
    port = int(port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', 0))
    # 等待 proxy 就绪：有回可连即可
    start = time.monotonic()
    while time.monotonic() - start < args.wait:
        try:
            sock.sendto(b'\x00' * 8, (host, port))
            time.sleep(0.1)
            break
        except OSError:
            time.sleep(0.2)

    templates = build_templates()
    stop = threading.Event()
    ok_count = [0]; fail_count = [0]

    def worker():
        while not stop.is_set():
            p = random.choice(templates)
            try:
                sock.sendto(p, (host, port))
                ok_count[0] += 1
            except OSError:
                fail_count[0] += 1
            time.sleep(1.0 / max(1, args.rate))
            # 非阻塞丢 reply（proxy 可能给攻击源回 reply，不处理也不影响 guest）
            try:
                sock.setblocking(False)
                while True:
                    _ = sock.recvfrom(4096)
            except OSError:
                pass
            finally:
                sock.setblocking(True)

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(args.threads)]
    for t in threads: t.start()
    time.sleep(args.duration)
    stop.set()
    for t in threads: t.join(timeout=2)

    print("== 虚拟 TCP 压力注入汇总 ==")
    print("  templates=%d  threads=%d  duration=%ds  rate/thread=%d/s" % (
        len(templates), args.threads, args.duration, args.rate))
    print("  send ok=%d  fail=%d" % (ok_count[0], fail_count[0]))
    print("  注入已停；合法链路 RESULT 断言由调用方判定")

if __name__ == '__main__':
    main()