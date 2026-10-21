// lz4_check.cpp —— 跨语言校验工具（主机侧，非测试）
// 用法：lz4_check <expected_len> <in_file> <out_file>
// 读取 in_file（LZ4 Block 流），解压并校验长度 == expected_len，写 out_file。
// 供 tools/make_packets.py 交叉验证固件解压器。退出码 0=成功。
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/lz4_block.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <expected_len> <in_file> <out_file>\n", argv[0]);
        return 2;
    }
    const size_t expected = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));

    std::FILE* in = std::fopen(argv[2], "rb");
    if (in == nullptr) {
        std::perror("open in");
        return 2;
    }
    std::vector<uint8_t> src;
    {
        uint8_t tmp[4096];
        size_t n;
        while ((n = std::fread(tmp, 1, sizeof(tmp), in)) != 0) {
            src.insert(src.end(), tmp, tmp + n);
        }
        std::fclose(in);
    }

    std::vector<uint8_t> dst(expected);
    const int r = ijpd::lz4_block_decompress(src.data(), src.size(), dst.data(),
                                             dst.size(), expected);
    if (r < 0) {
        std::fprintf(stderr, "decompress failed: %d\n", r);
        return 1;
    }

    std::FILE* out = std::fopen(argv[3], "wb");
    if (out == nullptr) {
        std::perror("open out");
        return 2;
    }
    if (expected != 0 && std::fwrite(dst.data(), 1, dst.size(), out) != dst.size()) {
        std::fclose(out);
        return 2;
    }
    std::fclose(out);
    return 0;
}
