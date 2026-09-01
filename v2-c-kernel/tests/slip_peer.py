#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/slip_peer.py
# 宿主 SLIP + UDP/IP 回显对端（路线图 v1.1 Step 2）。
# 连接 QEMU COM2 的 TCP 串口对端（QEMU 为 server），做 SLIP(RFC 1055) 解帧，
# 对每个 IP 数据报：若为 UDP 且载荷以 "PING" 开头，则交换四元组(src/dst ip+port)、
# 重算 IP/UDP 校验和，返回 "PONG..." 载荷，SLIP 再封装写回（guest 经串口网卡收到）。
# 用法: slip_peer.py <host> <port> <log>
# 日志每回显一次写一行 "PING->PONG"，供回归脚本断言。
import socket, sys

END = 0xC0
ESC = 0xDB

conn = None
def respond(frame):
    if len(frame) < 28:
        return
    ihl = (frame[0] & 0x0F) * 4
    if ihl < 20 or ihl > len(frame):
        return
    if frame[9] != 17:          # 只处理 UDP (proto 17)
        return
    udp = frame[ihl:]
    if len(udp) < 8:
        return
    ulen = int.from_bytes(udp[4:6], 'big')
    if ulen < 8 or 8 + ihl > len(frame):
        return
    payload = udp[8:ulen]
    if payload[:4] != b'PING':
        return
    # 交换四元组：src<->dst IP、src<->dst 端口；载荷回答 PONG
    sip = frame[12:16]; dip = frame[16:20]
    r_ip = bytearray(frame[:ihl])
    r_ip[12:16] = dip            # 应答源 = 原目的
    r_ip[16:20] = sip            # 应答目的 = 原源
    resp_payload = b'PONG' + payload[4:]
    r_udp = bytearray(8 + len(resp_payload))
    r_udp[0:2] = udp[2:4]        # src port = 原 dst port
    r_udp[2:4] = udp[0:2]        # dst port = 原 src port
    r_udp[4:6] = (8 + len(resp_payload)).to_bytes(2, 'big')
    r_udp[8:] = resp_payload      # 载荷写入（校验位此时为 0）
    # UDP 校验和（伪头 + UDP头(校验位0) + 载荷），RFC 768
    pseudo = dip + sip + b'\x00' + bytes([17]) + r_udp[4:6]
    cs = ip_sum(pseudo + bytes(r_udp))
    if cs == 0:
        cs = 0xFFFF
    r_udp[6:8] = cs.to_bytes(2, 'big')
    # IP 头总长 + 校验和
    r_ip[2:4] = (ihl + 8 + len(resp_payload)).to_bytes(2, 'big')
    r_ip[10:12] = b'\x00\x00'
    r_ip[10:12] = ip_sum(bytes(r_ip[:ihl])).to_bytes(2, 'big')
    full = bytes(r_ip) + bytes(r_udp)
    conn.sendall(slip(full))

def ip_sum(data):               # RFC 1071 16-bit ones-complement
    if len(data) & 1:
        data += b'\x00'
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

def slip(frame):
    out = bytearray([END])
    for b in frame:
        if b == END:
            out += bytes([ESC, 0xDC])
        elif b == ESC:
            out += bytes([ESC, 0xDD])
        else:
            out.append(b)
    out.append(END)
    return bytes(out)

def main():
    host, port, log = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    global conn
    completed = 0
    f = open(log, 'w')
    # 重连直至 QEMU server 就绪
    for _ in range(100):
        try:
            conn = socket.create_connection((host, port), timeout=2)
            conn.settimeout(120)
            break
        except OSError:
            import time; time.sleep(0.1)
    else:
        f.write('CONNECT_FAIL\n'); f.flush(); sys.exit(2)
    f.write('CONNECTED\n'); f.flush()
    buf = b''; frame = b''; esc = False
    while True:
        try:
            d = conn.recv(4096)
        except socket.timeout:
            break
        if not d:
            break
        buf += d
        while buf:
            b = buf[0]; buf = buf[1:]
            if esc:
                esc = False
                if b == 0xDC: frame += b'\xC0'
                elif b == 0xDD: frame += b'\xDB'
                else: frame = b''
            elif b == ESC:
                esc = True
            elif b == END:
                if frame:
                    respond(frame)
                    completed += 1
                    f.write('PING->PONG\n'); f.flush()
                frame = b''
            else:
                frame += bytes([b])
    f.close()
    sys.exit(0)

if __name__ == '__main__':
    main()