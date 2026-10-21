// frame_splitter.cpp —— 有界 '#' 拆帧
#include "core/frame_splitter.h"

namespace ijpd {

FrameSplitter::Stats FrameSplitter::feed(const uint8_t* data, size_t len,
                                         Callback cb, void* user) noexcept {
    Stats st{0, 0, 0};
    const size_t cap = capacity();

    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];
        if (c == static_cast<uint8_t>(cfg::FRAME_TERMINATOR)) {
            if (overflow_) {
                ++st.too_large;
            } else if (len_ == 0) {
                ++st.empty_ignored;
            } else {
                if (cb != nullptr) {
                    cb(user, buf_, len_);
                }
                ++st.frames;
            }
            len_ = 0;
            overflow_ = false;
            continue;
        }
        if (!overflow_) {
            if (len_ < cap) {
                buf_[len_++] = static_cast<char>(c);
            } else {
                overflow_ = true;  // 超限：停止存储，等待 '#' 复位，缓冲不增长
            }
        }
    }
    return st;
}

}  // namespace ijpd
