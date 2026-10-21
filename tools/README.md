# tools/ —— 主机侧开发工具（仅 Python 标准库，不入固件产物）

## make_packets.py

测试用二进制分包生成器，实现：

- **LZ4 Block 编码器**（贪心哈希匹配）：仅用于生成测试向量与回环自检；
- **LZ4 Block 解码器**：语义与固件 `components/core/src/lz4_block.cpp` 一致，用于交叉对照；
- **帧构造**：Hello（12B）+ 分包帧（协议 §6，含 Little Endian、nominal/last 包长规则、
  存储兜底（`clen==ulen` 即原始数据）、可选 CRC32）；
- **数据集生成**：random / runs / zeros / mixed 四种图案。

### selftest（回环 + 帧流重建 + 可选固件解压器交叉验证）

```bash
python3 tools/make_packets.py selftest
# 交叉验证（需先构建 host_tests，产出 lz4_check）：
python3 tools/make_packets.py selftest --decoder host_tests/build/lz4_check
```

### gen（生成任务流文件，供手工测试 / Phase 6 send_job.py 参考）

```bash
python3 tools/make_packets.py gen --out job.bin --bytes 120000 --packets 30 \
    --compression lz4_block --crc32 --pattern mixed --token 0x12345678
```

输出文件布局：`Hello(12B) | Frame0 | Frame1 | ... | FrameN-1`。

## send_job.py（Phase 6 提供）

控制+数据双通道模拟客户端（socket/struct/json/zlib.crc32）。
