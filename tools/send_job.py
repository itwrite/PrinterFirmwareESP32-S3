#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""send_job.py —— 控制+数据双通道模拟客户端（仅 Python 标准库，Phase 6）

流程（协议 docs/protocol_v1.md）：
  1. TCP 控制（9100）：发送 start_print，解析 data_token / nominal_packet_bytes
  2. TCP 数据（9101）：Hello 握手 + 分包发送（LZ4/存储兜底/CRC32）
  3. select 双向：边发数据边收 print_progress / print_finished 事件

用法示例：
  python3 tools/send_job.py --host 192.168.1.50 --bytes 120000 --packets 30
  python3 tools/send_job.py --cancel-after 10          # 第 10 包后取消
  python3 tools/send_job.py --corrupt-crc              # 末包 CRC 损坏
  python3 tools/send_job.py --break-after 12           # 第 12 包后断开数据连接
  python3 tools/send_job.py --never-connect            # 不连数据通道（测超时）
"""
import argparse
import json
import random
import select
import socket
import struct
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from make_packets import (build_frame, build_hello, gen_dataset,  # noqa: E402
                          nominal_lengths)


def ctrl_send(sock, msg_id, cmd, data):
    frame = json.dumps({"msg_id": msg_id, "cmd": cmd, "data": data},
                       separators=(",", ":")) + "#"
    sock.sendall(frame.encode())


def ctrl_recv(sock, timeout=2.0):
    """读取一条 '#' 结尾的响应(跳过空帧/残留定界符)。"""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        while b"#" in buf:
            raw, _, buf = buf.partition(b"#")
            if raw.strip():
                try:
                    return json.loads(raw.decode()), buf
                except json.JSONDecodeError:
                    print("DEBUG bad frame:", repr(raw[:160]))
                    raise
        r, _, _ = select.select([sock], [], [], 0.1)
        if not r:
            continue
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return None, buf


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="192.168.1.50")
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--job", default="job-001")
    ap.add_argument("--bytes", type=int, default=120000)
    ap.add_argument("--packets", type=int, default=30)
    ap.add_argument("--compression", choices=["none", "lz4_block"],
                    default="lz4_block")
    ap.add_argument("--no-crc", action="store_true")
    ap.add_argument("--pattern", choices=["random", "runs", "zeros", "mixed"],
                    default="mixed")
    ap.add_argument("--bpr", type=int, default=None,
                    help="bytes_per_row（默认 total/100，需整除）")
    ap.add_argument("--cancel-after", type=int, default=0)
    ap.add_argument("--corrupt-crc", action="store_true")
    ap.add_argument("--break-after", type=int, default=0)
    ap.add_argument("--never-connect", action="store_true")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    total = args.bytes
    packets = args.packets
    bpr = args.bpr if args.bpr else (total // 100 if total % 100 == 0 else None)
    if bpr is None or total % bpr != 0:
        print(f"ERROR: total({total}) 必须能被 bpr 整除（--bpr 指定）")
        return 2
    rows = total // bpr
    nominal, last = nominal_lengths(total, packets)
    print(f"job: {total}B / {packets} pkts / bpr={bpr} rows={rows} "
          f"nominal={nominal} last={last}")

    # ---- 控制连接 ----
    ctrl = socket.create_connection((args.host, args.port), timeout=5)
    ctrl_send(ctrl, "job-create-001", "start_print", {
        "job_id": args.job,
        "total_bytes_uncompressed": total,
        "total_packets": packets,
        "compression": args.compression,
        "integrity": "none" if args.no_crc else "crc32",
        "bytes_per_row": bpr,
        "total_rows": rows,
        "cutter_after_finish": False,
    })
    resp, _ = ctrl_recv(ctrl, 5.0)
    if resp is None:
        print("ERROR: 无 start_print 响应")
        return 1
    if not resp["data"]["success"]:
        print("ERROR:", json.dumps(resp["data"]["error"], ensure_ascii=False))
        return 1
    result = resp["data"]["result"]
    token = result["data_token"]
    print(f"start_print ok: token=0x{token:08X} nominal={result['nominal_packet_bytes']}")
    assert result["nominal_packet_bytes"] == nominal, "nominal 不一致"

    if args.never_connect:
        print("等待数据连接超时事件（10s）...")
        ev, _ = ctrl_recv(ctrl, 12.0)
        print("event:", json.dumps(ev, ensure_ascii=False) if ev else "None")
        ctrl.close()
        return 0

    # ---- 数据连接 ----
    data = socket.create_connection((args.host, result["data_port"]), timeout=5)
    data.sendall(build_hello(token))

    dataset = gen_dataset(args.pattern, total, rng)
    use_crc = not args.no_crc
    frames = []
    for k in range(packets):
        ulen = last if k == packets - 1 else nominal
        raw = dataset[k * nominal:k * nominal + ulen]
        frames.append(build_frame(k, raw, args.compression, use_crc))
    if args.corrupt_crc:
        f = bytearray(frames[-1])
        f[-1] ^= 0x01  # 翻转 CRC 最低位
        frames[-1] = bytes(f)

    # ---- 双向 select 循环 ----
    pending = b"".join(frames)
    sent_pkts = 0
    break_done = False
    cancel_sent = False
    t0 = time.time()
    final = None

    while True:
        rlist, wlist, _ = select.select([ctrl], [data] if pending else [], [], 0.05)
        if wlist and pending:
            n = data.send(pending[:4096])
            pending = pending[n:]
            sent = sum(len(f) for f in frames) - len(pending)
            # 粗略包计数（按已发送字节推进）
            while sent_pkts < packets and sent >= sum(
                    len(f) for f in frames[:sent_pkts + 1]):
                sent_pkts += 1
                if args.cancel_after and sent_pkts == args.cancel_after and not cancel_sent:
                    ctrl_send(ctrl, "cancel-1", "cancel_print", {})
                    cancel_sent = True
                    print(f"cancel_print @pkt{sent_pkts}")
                if args.break_after and sent_pkts == args.break_after and not break_done:
                    break_done = True
                    print(f"断开数据连接 @pkt{sent_pkts}")
                    data.close()
                    pending = b""
        if rlist:
            ev, _ = ctrl_recv(ctrl, 1.0)
            if ev:
                cmd = ev.get("cmd")
                if cmd == "print_progress":
                    s = ev["data"]["result"]
                    print(f"progress: rx={s['packets_received']} "
                          f"dec={s['packets_decoded']} wm={s['queue_watermark']} "
                          f"{s['throughput_bps']}B/s")
                elif cmd == "print_finished":
                    final = ev
                    break
        if not pending and data.fileno() >= 0 and not final:
            # 发送完毕：等待终态事件（最多 15s）
            if time.time() - t0 > 30:
                print("TIMEOUT: 等待 print_finished")
                break
        if data.fileno() < 0 and not final and time.time() - t0 > 12:
            print("断开后未收到终态事件")
            break

    dt = time.time() - t0
    if final:
        d = final["data"]
        print(f"print_finished: success={d['success']} "
              f"result/error={json.dumps(d.get('result') or d.get('error'), ensure_ascii=False)}")
    print(f"耗时 {dt:.2f}s，吞吐 ~{total / max(dt, 0.001) / 1000:.0f}KB/s")
    try:
        data.close()
    except OSError:
        pass
    ctrl.close()
    return 0 if final else 1


if __name__ == "__main__":
    sys.exit(main())
