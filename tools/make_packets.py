#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_packets.py —— 测试用二进制分包生成器（仅 Python 标准库，Phase 3）

职责：
  1. 自带 LZ4 Block 编码器（贪心哈希匹配）与解码器（用于自检回环）；
  2. 按 docs/protocol_v1.md §6 构造 Hello 与分包帧（含存储兜底、可选 CRC32）；
  3. selftest：编码/解码回环 + 帧流重建校验；可选用 --decoder 交叉验证固件
     解压器（host_tests/lz4_check）。

用法示例：
  python3 tools/make_packets.py selftest
  python3 tools/make_packets.py selftest --decoder host_tests/build/lz4_check
  python3 tools/make_packets.py gen --out job.bin --bytes 120000 --packets 30 \
      --compression lz4_block --crc32 --pattern mixed --token 305419896
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile
import zlib

MAGIC = b"IJPD"
HELLO_VERSION = 0x0001
MINMATCH = 4
MFLIMIT = 12
HASH_LOG = 12
HASH_SIZE = 1 << HASH_LOG


def crc32(b: bytes) -> int:
    return zlib.crc32(b) & 0xFFFFFFFF


# ----------------------------------------------------------------------------
# LZ4 Block 编码器（贪心哈希）
# ----------------------------------------------------------------------------

def _hash4(buf: bytes, p: int) -> int:
    v = buf[p] | (buf[p + 1] << 8) | (buf[p + 2] << 16) | (buf[p + 3] << 24)
    return (v * 2654435761) & (HASH_SIZE - 1)


def _emit(out: bytearray, buf: bytes, lit_start: int, lit_len: int,
          offset: int, m4: int) -> None:
    ln = lit_len if lit_len < 15 else 15
    mn = m4 if m4 < 15 else 15
    out.append((ln << 4) | mn)
    if ln == 15:
        r = lit_len - 15
        while r >= 255:
            out.append(255)
            r -= 255
        out.append(r)
    out += buf[lit_start:lit_start + lit_len]
    if offset != 0:  # 0 = 末尾 literals-only 序列
        out += struct.pack("<H", offset)
        if mn == 15:
            r = m4 - 15
            while r >= 255:
                out.append(255)
                r -= 255
            out.append(r)


def lz4_compress(src: bytes) -> bytes:
    n = len(src)
    out = bytearray()
    table = [-1] * HASH_SIZE
    anchor = ip = 0
    match_limit = n - MFLIMIT if n >= MFLIMIT else 0
    while ip < match_limit:
        h = _hash4(src, ip)
        ref = table[h]
        table[h] = ip
        if ref >= 0 and ip - ref <= 0xFFFF and src[ref:ref + 4] == src[ip:ip + 4]:
            ml = 4
            while ip + ml + 1 < n and src[ref + ml] == src[ip + ml]:
                ml += 1
            _emit(out, src, anchor, ip - anchor, ip - ref, ml - MINMATCH)
            ip += ml
            anchor = ip
        else:
            ip += 1
    _emit(out, src, anchor, n - anchor, 0, 0)  # 尾部 literals（>= 1）
    return bytes(out)


# ----------------------------------------------------------------------------
# LZ4 Block 解码器（自检用；语义与固件 components/core/src/lz4_block.cpp 一致）
# ----------------------------------------------------------------------------

class Lz4Error(Exception):
    pass


def lz4_decompress(src: bytes, expected_len: int) -> bytes:
    n = len(src)
    out = bytearray()
    s = 0
    while True:
        if s >= n:
            raise Lz4Error("need token")
        token = src[s]
        s += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if s >= n:
                    raise Lz4Error("lit ext truncated")
                b = src[s]
                s += 1
                lit += b
                if b != 255:
                    break
        if lit > n - s or lit > expected_len - len(out):
            raise Lz4Error("literal out of range")
        out += src[s:s + lit]
        s += lit
        if s == n:
            break
        if n - s < 2:
            raise Lz4Error("need offset")
        offset = src[s] | (src[s + 1] << 8)
        s += 2
        if offset == 0 or offset > len(out):
            raise Lz4Error("bad offset")
        mlen = (token & 0x0F) + 4
        if (token & 0x0F) == 15:
            while True:
                if s >= n:
                    raise Lz4Error("match ext truncated")
                b = src[s]
                s += 1
                mlen += b
                if mlen > expected_len:
                    raise Lz4Error("match too long")
                if b != 255:
                    break
        if len(out) + mlen > expected_len:
            raise Lz4Error("match overflow")
        base = len(out) - offset
        for i in range(mlen):  # 逐字节：重叠安全
            out.append(out[base + i])
    if len(out) != expected_len:
        raise Lz4Error("length mismatch")
    return bytes(out)


# ----------------------------------------------------------------------------
# 分包构造（协议 §6）
# ----------------------------------------------------------------------------

def build_hello(token: int) -> bytes:
    return MAGIC + struct.pack("<HIH", HELLO_VERSION, token, 0)


def nominal_lengths(total_bytes: int, total_packets: int):
    nominal = (total_bytes + total_packets - 1) // total_packets
    last = total_bytes - nominal * (total_packets - 1)
    assert 1 <= last <= nominal
    return nominal, last


def build_frame(index: int, raw: bytes, compression: str, use_crc: bool) -> bytes:
    ulen = len(raw)
    if compression == "lz4_block":
        comp = lz4_compress(raw)
        if len(comp) >= ulen:
            comp = raw  # 存储兜底：clen == ulen 表示原始数据
    else:
        comp = raw
    body = struct.pack("<HHH", index, ulen, len(comp)) + comp
    if use_crc:
        body += struct.pack("<I", crc32(body))
    return body


def split_job(data: bytes, total_packets: int):
    """按 nominal/last 切分原始数据。"""
    nominal, last = nominal_lengths(len(data), total_packets)
    chunks = []
    for k in range(total_packets):
        n = last if k == total_packets - 1 else nominal
        chunks.append(data[k * nominal:k * nominal + n])
    assert b"".join(chunks) == data
    return chunks


def build_job_stream(data: bytes, total_packets: int, compression: str,
                     use_crc: bool, token: int) -> bytes:
    stream = build_hello(token)
    for k, chunk in enumerate(split_job(data, total_packets)):
        stream += build_frame(k, chunk, compression, use_crc)
    return stream


def gen_dataset(kind: str, size: int, rng: random.Random) -> bytes:
    if kind == "random":
        return bytes(rng.getrandbits(8) for _ in range(size))
    if kind == "runs":
        return bytes((i // 64) & 0xFF for i in range(size))
    if kind == "zeros":
        return b"\x00" * size
    if kind == "mixed":
        return bytes((rng.getrandbits(8) & 0x03) * 0x55 if i % 8 == 0
                     else rng.getrandbits(8) for i in range(size))
    raise ValueError(kind)


# ----------------------------------------------------------------------------
# selftest
# ----------------------------------------------------------------------------

def _cross_check_decoder(decoder: str, data: bytes, comp: bytes) -> bool:
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "in.bin")
        dst = os.path.join(td, "out.bin")
        with open(src, "wb") as f:
            f.write(comp)
        r = subprocess.run([decoder, str(len(data)), src, dst],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("  decoder failed:", r.stderr.strip())
            return False
        with open(dst, "rb") as f:
            got = f.read()
        return got == data


def selftest(decoder=None, iterations=None) -> int:
    rng = random.Random(20261015)
    sizes = [1, 2, 3, 15, 16, 100, 255, 256, 1000, 4096, 10000]
    kinds = ["random", "runs", "zeros", "mixed"]
    n_case = 0
    n_fail = 0

    for kind in kinds:
        for size in sizes:
            data = gen_dataset(kind, size, rng)
            comp = lz4_compress(data)
            back = lz4_decompress(comp, len(data))
            ok = (back == data)
            # 可压缩数据应确实小于原数据（runs/zeros；小数据天然无法压缩，
            # 走存储兜底属正常）
            if kind in ("runs", "zeros") and size >= 100 and len(comp) >= len(data):
                ok = False
            if decoder:
                ok = ok and _cross_check_decoder(decoder, data, comp)
            n_case += 1
            if not ok:
                n_fail += 1
                print(f"  FAIL lz4 roundtrip kind={kind} size={size}")
    # 大数据集（runs 一次 65535）
    data = gen_dataset("runs", 65535, rng)
    comp = lz4_compress(data)
    ok = lz4_decompress(comp, len(data)) == data
    if decoder:
        ok = ok and _cross_check_decoder(decoder, data, comp)
    n_case += 1
    if not ok:
        n_fail += 1
        print("  FAIL lz4 roundtrip kind=runs size=65535")

    # 帧流构建 + 重建校验（任意参数组合）
    for compression in ("none", "lz4_block"):
        for use_crc in (False, True):
            total_packets = rng.randint(1, 30)
            total_bytes = rng.randint(total_packets, 120000)
            nominal, last = nominal_lengths(total_bytes, total_packets)
            data = gen_dataset("mixed", total_bytes, rng)
            stream = build_job_stream(data, total_packets, compression, use_crc,
                                      0x0123ABCD)
            # 重建
            token = rng.randint(1, 0xFFFFFFFF)
            stream = build_job_stream(data, total_packets, compression, use_crc, token)
            assert stream[:4] == MAGIC
            assert struct.unpack("<HIH", stream[4:12]) == (HELLO_VERSION, token, 0)
            pos = 12
            rebuilt = b""
            for k in range(total_packets):
                idx, ulen, clen = struct.unpack("<HHH", stream[pos:pos + 6])
                pos += 6
                payload = stream[pos:pos + clen]
                pos += clen
                if use_crc:
                    crc = struct.unpack("<I", stream[pos:pos + 4])
                    pos += 4
                    body = stream[pos - clen - (4 if use_crc else 0) - 6:pos - 4]
                    assert crc[0] == crc32(body), "CRC mismatch in built stream"
                assert idx == k
                want = last if k == total_packets - 1 else nominal
                assert ulen == want
                if compression == "lz4_block":
                    rebuilt += payload if clen == ulen else lz4_decompress(payload, ulen)
                else:
                    assert clen == ulen
                    rebuilt += payload
            n_case += 1
            if rebuilt != data:
                n_fail += 1
                print(f"  FAIL stream rebuild compression={compression} crc={use_crc}")

    # 存储兜底：随机数据（不可压缩）必须走 clen==ulen 原始路径
    data = gen_dataset("random", 2000, rng)
    frame = build_frame(0, data, "lz4_block", False)
    idx, ulen, clen = struct.unpack("<HHH", frame[:6])
    n_case += 1
    if clen != ulen or frame[6:6 + clen] != data:
        n_fail += 1
        print("  FAIL store-fallback for incompressible data")

    print(f"make_packets selftest: {n_case} cases, {n_fail} failed"
          + (" (decoder: " + decoder + ")" if decoder else ""))
    return 0 if n_fail == 0 else 1


# ----------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    st = sub.add_parser("selftest", help="编码/解码回环与帧流重建自检")
    st.add_argument("--decoder", default=None,
                    help="固件解压器交叉验证工具（lz4_check）路径")

    g = sub.add_parser("gen", help="生成任务数据流文件")
    g.add_argument("--out", required=True)
    g.add_argument("--bytes", type=int, required=True)
    g.add_argument("--packets", type=int, required=True)
    g.add_argument("--compression", choices=["none", "lz4_block"], default="lz4_block")
    g.add_argument("--crc32", action="store_true")
    g.add_argument("--pattern", choices=["random", "runs", "zeros", "mixed"],
                   default="mixed")
    g.add_argument("--token", type=lambda x: int(x, 0), default=0x12345678)
    g.add_argument("--seed", type=int, default=1)

    args = ap.parse_args()
    if args.cmd == "selftest":
        return selftest(args.decoder)

    rng = random.Random(args.seed)
    data = gen_dataset(args.pattern, args.bytes, rng)
    stream = build_job_stream(data, args.packets, args.compression, args.crc32,
                              args.token)
    with open(args.out, "wb") as f:
        f.write(stream)
    nominal, last = nominal_lengths(args.bytes, args.packets)
    print(f"wrote {args.out}: {len(stream)} bytes")
    print(f"job: total_bytes={args.bytes} packets={args.packets} "
          f"nominal={nominal} last={last} compression={args.compression} "
          f"crc32={args.crc32} token={args.token}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
