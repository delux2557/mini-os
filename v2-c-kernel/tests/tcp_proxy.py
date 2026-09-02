#!/usr/bin/env python3
# mini-os/v2-c-kernel/tests/tcp_proxy.py
# 虚拟 TCP 宿主转发器（netif Step 4）。真 TCP 状态机只在宿主：guest 经会话协议头
# （docs/tcp-session-proto.md v1.1）与其交互，本进程把 UDP 会话映射到真实 TCP 连接。
# 支持两个输入通道（双通道都要跑）：
#   --mode udp PORT   : 绑定 UDP PORT 收客机会话数据报（e1000/SLIRP 路径）
#   --mode slip H P   : 连 QEMU COM2 的 TCP 串口对端，SLIP 解帧 + IP/UDP 解析（串口路径）
# 会话表 {session_id: Session}；MSG_OPEN/CLOSE/DATA 处理 + 事件回传 + 空闲超时清理。
# 用法: tcp_proxy.py --mode udp --port 7778 --target 127.0.0.1:8080 [--log f] [--idle N]
#       tcp_proxy.py --mode slip --host 127.0.0.1 --port 7901 --target 127.0.0.1:8080
import socket, sys, threading, time, select, argparse

END, ESC = 0xC0, 0xDB
HDR = 8
MSG_DATA, MSG_OPENED, MSG_CLOSED, MSG_ERROR, MSG_TIMEOUT, MSG_OPEN, MSG_CLOSE, MSG_ACK = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08)
PROTO_VERSION = 1

logf = None
def log(msg):
    if logf:
        try:
            logf.write(msg + '\n'); logf.flush()
        except Exception:
            pass

# ---------------- 会话数据报编/解码 ----------------
def hdr(sid, mtype):
    return sid.to_bytes(4, 'big') + bytes([mtype, PROTO_VERSION, 0, 0])

def parse_hdr(pkt):
    if len(pkt) < HDR: return None
    if pkt[5] != PROTO_VERSION: return None
    sid = int.from_bytes(pkt[0:4], 'big')
    seq = (pkt[6] << 8) | pkt[7]          # flags 低16位 = 该方向的可靠 seq（DATA 用）
    return (sid, pkt[4], seq)

# ---------------- SLIP (RFC 1055) ----------------
def slip_tx(frame):
    out = bytearray([END])
    for b in frame:
        if b == END: out += bytes([ESC, 0xDC])
        elif b == ESC: out += bytes([ESC, 0xDD])
        else: out.append(b)
    out.append(END); return bytes(out)

class SlipDecoder:
    def __init__(self): self.buf = b''; self.frame = b''; self.esc = False
    def feed(self, data):
        self.buf += data; frames = []
        while self.buf:
            b = self.buf[0]; self.buf = self.buf[1:]
            if self.esc:
                self.esc = False
                if b == 0xDC: self.frame += b'\xC0'
                elif b == 0xDD: self.frame += b'\xDB'
                else: self.frame = b''
            elif b == ESC: self.esc = True
            elif b == END:
                if self.frame: frames.append(self.frame)
                self.frame = b''
            else: self.frame += bytes([b])
        return frames

def ip_checksum(data):
    if len(data) & 1: data += b'\x00'
    s = 0
    for i in range(0, len(data), 2): s += (data[i] << 8) | data[i + 1]
    while s >> 16: s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

# 从 UDP-over-IPv4 数据报中剥出会话载荷 + 源 addr
def udp_session(frame):
    if len(frame) < 28: return None
    ihl = (frame[0] & 0x0F) * 4
    if ihl < 20 or ihl > len(frame): return None
    if frame[9] != 17: return None                 # UDP proto
    sip = frame[12:16]; dip = frame[16:20]
    udp = frame[ihl:]
    if len(udp) < 8: return None
    ulen = int.from_bytes(udp[4:6], 'big')
    if ulen < 8 or ihl + ulen > len(frame): return None
    payload = udp[8:ulen]
    src_port = int.from_bytes(udp[0:2], 'big')
    dst_port = int.from_bytes(udp[2:4], 'big')
    return (sip, src_port, dip, dst_port, payload)

def make_udp(sip, sport, dip, dport, payload):
    udp = bytearray(8 + len(payload))
    udp[0:2] = sport.to_bytes(2, 'big'); udp[2:4] = dport.to_bytes(2, 'big')
    udp[4:6] = (8 + len(payload)).to_bytes(2, 'big')
    udp[8:] = payload
    pseudo = sip + dip + b'\x00' + bytes([17]) + udp[4:6]
    cs = ip_checksum(pseudo + bytes(udp))
    if cs == 0: cs = 0xFFFF
    udp[6:8] = cs.to_bytes(2, 'big')
    ip = bytearray(ihl := 20)
    ip[0] = 0x45
    ip[2:4] = (ihl + len(udp)).to_bytes(2, 'big')
    ip[8] = 64; ip[9] = 17; ip[12:16] = dip; ip[16:20] = sip
    ip[10:12] = ip_checksum(bytes(ip[:ihl])).to_bytes(2, 'big')
    return bytes(ip) + bytes(udp)

# ---------------- 会话 / 转发逻辑 ----------------
# v1.2 可靠下行（stop-and-wait）：host→guest 的 MSG_DATA 每报带递增 seq，代理每会话
# 只有 ≤1 个数据报在途（ACK 才发下一个）；ACK 丢则按 RETX_MS 定时重发该报，guest 幂等
# 丢弃重发并回 ACK，自愈。彻底消除 burst 在 NIC/socket 界面的随机丢包，128KB 大文件不缺尾。
class Session:
    def __init__(self, sid, peer, addr, target):
        self.sid = sid; self.peer = peer
        self.addr = addr                    # guest 侧 addr（回传目的）
        self.tcp = None
        self.state = 'OPENING'
        self.last = time.monotonic()
        self.target = target
        self.closed = False
        # ---- 可靠下行（stop-and-wait）状态 ----
        self.pending = bytearray()          # host TCP 读到的、尚未分派到数据报的待发字节
        self.seq = 0                        # 下一个要分配的下行序列号
        self.inflight = None                # 在途数据报载荷（等 ACK）；None=空闲
        self.inflight_seq = None
        self.inflight_t = 0.0
        self.eof = False                    # host TCP 已 EOF（pending 发完后可关）
        self.finished = False               # 已发 MSG_CLOSED
        # ---- 可靠上行（stop-and-wait，v1.2 上行可靠）状态 ----
        self.up_next = 0                    # 下一个期望的上行 DATA 序列号（guest→host）
    def touch(self): self.last = time.monotonic()

class Proxy:
    # 可靠下行重传间隔（秒）：需 ≥ 最慢通道的单报回环。SLIP 下 guest UART 收 1400B 数据报
    # 约需 ~1s（100Hz/16B FIFO），RTX 过短会在 ACK 到前疯狂重发灌爆慢 UART（test_tcp 串口通道
    # 实测 60ms 卡死）；e1000 回环 <10ms，正常无丢不触发重传，2s 只在真丢时用于恢复。
    RETX_MS = 2.0
    CHUNK   = 1392          # 单数据报载荷上限（= netsock 每数据报钳制上限-8）

    def __init__(self, logfile=None, idle=30.0, timeout=8.0):
        self.sess = {}
        self.idle = idle; self.timeout = timeout
        global logf; logf = open(logfile, 'w') if logfile else None

    def reply(self, sid, mtype, payload=b'', addr=None):
        s = self.sess.get(sid)
        a = addr if addr is not None else (s.addr if s else None)
        if a is None: return
        self.peer_send(a, hdr(sid, mtype) + payload)
        log(f'send sid={sid} type={mtype} -> {a}')

    # 发一个带 seq 的下行数据报（payload 必须 ≤CHUNK）
    def send_data(self, sess, seq, payload):
        h = bytearray(hdr(sess.sid, MSG_DATA))
        h[6] = (seq >> 8) & 0xFF; h[7] = seq & 0xFF
        self.peer_send(sess.addr, bytes(h) + payload)
        log(f'send sid={sess.sid} data seq={seq} {len(payload)}B -> {sess.addr}')

    # 推进一次发送（stop-and-wait）：空闲才填一个数据报；pending 发尽且 eof 则发 CLOSED
    def _send_next(self, sess):
        if sess.finished: return
        if sess.state == 'CLOSING': return
        if sess.inflight is not None:        # 上一个还没 ACK，不能再发
            return
        if len(sess.pending) > 0:
            payload = bytes(sess.pending[:self.CHUNK])
            del sess.pending[:len(payload)]
            sess.inflight_seq = sess.seq
            sess.seq = (sess.seq + 1) & 0xFFFF
            sess.inflight = payload
            sess.inflight_t = time.monotonic()
            self.send_data(sess, sess.inflight_seq, payload)
        elif sess.eof:
            sess.finished = True
            sess.state = 'CLOSING'          # 数据全送达后才告知 guest 关闭（保证不缺尾）
            self.reply(sess.sid, MSG_CLOSED)
            self._teardown(sess)

    # 超时重传在途数据报（ACK 丢失自愈）。幂等：guest 收到重复 seq 丢弃并回 ACK
    def _retransmit(self, sess):
        if sess.inflight is not None and (time.monotonic() - sess.inflight_t) >= self.RETX_MS:
            sess.inflight_t = time.monotonic()
            self.send_data(sess, sess.inflight_seq, sess.inflight)

    def handle_msg(self, addr, mtype, sid, seq, payload):
        s = self.sess.get(sid)
        if s: s.touch()
        if mtype == MSG_OPEN:
            if len(payload) < 6:
                log(f'drop MSG_OPEN sid={sid} short={len(payload)}'); return
            if sid in self.sess:
                self._teardown(self.sess[sid]); del self.sess[sid]
            ip = socket.inet_ntoa(payload[0:4]); port = int.from_bytes(payload[4:6], 'big')
            log(f'MSG_OPEN sid={sid} -> TCP {ip}:{port} from {addr}')
            sess = Session(sid, self, addr, (ip, port))
            self.sess[sid] = sess
            t = threading.Thread(target=self._connect, args=(sess,), daemon=True)
            t.start()
        elif mtype == MSG_DATA:        # guest→host 上传：可靠上行（stop-and-wait，v1.2）
            if s and s.state == 'OPEN' and s.tcp:
                if seq == s.up_next:            # 顺序包：转发到真 TCP，回 ACK
                    try: s.tcp.sendall(payload)
                    except OSError: self._teardown(s)
                    s.up_next = (s.up_next + 1) & 0xFFFF
                    self._up_ack(s)
                else:                           # 重发/乱序：丢弃载荷，重发 ACK 让 guest 前进
                    self._up_ack(s)
        elif mtype == MSG_ACK:         # guest→host 累计 ACK：payload= 下一期望下行 seq(2BE)
            if s and len(payload) >= 2 and s.inflight is not None:
                ack = int.from_bytes(payload[0:2], 'big')
                # 期望下一个 == 在途 seq+1 → 该报已被接收，清空在途，发下一个
                if ack == ((s.inflight_seq + 1) & 0xFFFF):
                    s.inflight = None; s.inflight_seq = None
                    self._send_next(s)
        elif mtype == MSG_CLOSE:
            log(f'MSG_CLOSE sid={sid}')
            if s: self._teardown(s)
            self.sess.pop(sid, None)

    # 可靠上行：向 guest 回累计 ACK（payload = 下一期望上行 seq，2BE）。
    # MSG_ACK 按方向双向复用：host→guest 的 MSG_ACK 语义即"上行确认"。
    def _up_ack(self, s):
        p = (s.up_next >> 8) & 0xFF, s.up_next & 0xFF
        self.peer_send(s.addr, hdr(s.sid, MSG_ACK) + bytes(p))
        log(f'send sid={s.sid} up-ACK next={s.up_next} -> {s.addr}')

    def _connect(self, sess):
        try:
            sock = socket.create_connection(sess.target, timeout=self.timeout)
            sock.setblocking(False)                  # 非阻塞：读不卡主循环（见 _tcp_read）
            sess.tcp = sock; sess.state = 'OPEN'
            self.reply(sess.sid, MSG_OPENED)
            log(f'OPENED sid={sess.sid}')
        except OSError as e:
            log(f'open fail sid={sess.sid}: {e}')
            self.reply(sess.sid, MSG_ERROR)
            self.sess.pop(sess.sid, None)

    def _teardown(self, sess):
        if sess.closed: return
        sess.closed = True
        if sess.tcp:
            try: sess.tcp.close()
            except OSError: pass

    def _sweep(self):
        now = time.monotonic()
        for sid in list(self.sess):
            s = self.sess[sid]
            # 可靠下行：定时重传在途数据报；EOF 且 pending 发尽时驱动发 CLOSED（无新数据/ACK 也能推进）
            self._retransmit(s)
            self._send_next(s)
            if s.state == 'OPENING' and now - s.last > self.timeout:
                self.reply(sid, MSG_TIMEOUT); self._teardown(s); del self.sess[sid]
            elif s.closed and now - s.last > 2:
                del self.sess[sid]
            elif now - s.last > self.idle:
                self._teardown(s); del self.sess[sid]

    # 从 select 就绪集中读 TCP 下行：非阻塞，绝不阻塞主循环（否则 chardev/udp 输入被饿死）。
    # v1.2 可靠下行：读来的字节进了 pending 后由 stop-and-wait 逐步按 ACK 下发，不再 burst。
    def _tcp_read(self, ready):
        for sid in list(self.sess):
            s = self.sess[sid]
            if s.tcp is None or s.tcp not in ready: continue
            try:
                data = s.tcp.recv(4096)
            except (BlockingIOError, socket.timeout):
                continue
            except OSError:
                data = b''
            s.touch()
            if data:
                s.pending += data
                self._send_next(s)
            elif s.state == 'OPEN' and s.eof is False:
                # host TCP EOF：标记 eof，pending 发尽且 ACK 齐后由 _send_next 一次性发 CLOSED
                s.eof = True
                self._send_next(s)

    def _readable_tcps(self):
        return [s.tcp for s in self.sess.values() if s.tcp is not None and s.state == 'OPEN']

    #### 通道实现：调用方须提供 peer_send(addr, pkt) 与 poll 循环 ####
    def run_udp(self, port):
        self.mode_is_slip = False
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        udp.bind(('127.0.0.1', port))
        udp.setblocking(False)
        def peer_send(addr, pkt): udp.sendto(pkt, addr)
        self.peer_send = peer_send
        log(f'proxy UDP listening :{port}')
        while True:
            rlist = [udp] + self._readable_tcps()
            r, _, _ = select.select(rlist, [], [], 1.0)
            if udp in r:
                try:
                    while True:
                        pkt, addr = udp.recvfrom(8192)
                        h = parse_hdr(pkt)
                        if h: self.handle_msg(addr, h[1], h[0], h[2], pkt[HDR:])
                except BlockingIOError: pass
            self._tcp_read(r)
            self._sweep()

    def run_slip(self, host, port):
        conn = None
        for _ in range(100):
            try:
                conn = socket.create_connection((host, port), timeout=2); break
            except OSError:
                time.sleep(0.1)
        if conn is None: sys.exit(2)
        log('proxy connected to COM2 chardev')
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        conn.setblocking(False)
        lock = threading.Lock()
        def peer_send(addr, pkt):
            # 串口通道：收到帧的元组 (sip, sport, dip, dport)；应答回 guest 须交换源/目的。
            sip, sport, dip, dport = addr
            frame = make_udp(dip, dport, sip, sport, pkt)   # src=原目的(代理), dst=原源(guest)
            try:
                with lock: conn.sendall(slip_tx(frame))
            except OSError:
                pass
        self.peer_send = peer_send
        self.mode_is_slip = True; self.SLIP_CAP = 500
        dec = SlipDecoder()
        while True:
            rlist = [conn] + self._readable_tcps()
            r, _, _ = select.select(rlist, [], [], 1.0)
            if conn in r:
                try: d = conn.recv(4096)
                except BlockingIOError: d = b''
                except OSError: break
                if not d: break
                for fr in dec.feed(d):
                    u = udp_session(fr)
                    if not u: continue
                    sip, sport, dip, dport, payload = u
                    h = parse_hdr(payload)
                    if h:
                        self.handle_msg((sip, sport, dip, dport), h[1], h[0], h[2], payload[HDR:])
            self._tcp_read(r)
            self._sweep()
        log('slip channel closed'); sys.exit(0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', choices=['udp', 'slip'], required=True)
    ap.add_argument('--port', type=int, default=7778)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--log')
    ap.add_argument('--idle', type=float, default=30.0)
    ap.add_argument('--timeout', type=float, default=8.0)
    a = ap.parse_args()
    p = Proxy(logfile=a.log, idle=a.idle, timeout=a.timeout)
    if a.mode == 'udp': p.run_udp(a.port)
    else: p.run_slip(a.host, a.port)

if __name__ == '__main__':
    main()