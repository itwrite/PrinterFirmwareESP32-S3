// control_plane.cpp —— 控制面汇入
#include "app/control_plane.h"

#include <cstring>

namespace ijpd {

Err ControlPlane::init(CommandRouter* router) noexcept {
    if (router == nullptr) return Err::INVALID_ARGUMENT;
    router_ = router;
    for (int i = 0; i < kMaxChannels; ++i) {
        splitters_[i].clear_partial();
        last_rx_ms_[i] = 0;
        sinks_[i] = Sink{};
    }
    return Err::OK;
}

void ControlPlane::set_sink(int ch, const Sink& sink) noexcept {
    if (ch < 0 || ch >= kMaxChannels) return;
    sinks_[ch] = sink;
}

void ControlPlane::detach(int ch) noexcept {
    if (ch < 0 || ch >= kMaxChannels) return;
    splitters_[ch].clear_partial();
    sinks_[ch] = Sink{};
    last_rx_ms_[ch] = 0;
}

void ControlPlane::broadcast(const uint8_t* data, size_t len) noexcept {
    for (int ch = 0; ch < kMaxChannels; ++ch) {
        if (sinks_[ch].send != nullptr) {
            (void)sinks_[ch].send(sinks_[ch].user, data, len);
        }
    }
}

bool ControlPlane::has_partial(int ch) const noexcept {
    return (ch >= 0 && ch < kMaxChannels) ? splitters_[ch].has_partial() : false;
}

void ControlPlane::feed(int ch, const uint8_t* data, size_t len,
                        uint64_t now_ms) noexcept {
    if (ch < 0 || ch >= kMaxChannels || router_ == nullptr || (data == nullptr && len != 0)) {
        return;
    }
    last_rx_ms_[ch] = now_ms;

    FrameCtx ctx{this, ch};
    const auto st = splitters_[ch].feed(
        data, len,
        [](void* user, const char* frame, size_t flen) {
            auto* c = static_cast<FrameCtx*>(user);
            c->self->on_frame_(c->ch, frame, flen);
        },
        &ctx);

    // 超长帧：逐帧回 FRAME_TOO_LARGE（msg_id=null）
    for (size_t i = 0; i < st.too_large; ++i) {
        respond_(ch, Err::FRAME_TOO_LARGE, "control frame exceeds the maximum size");
    }
}

void ControlPlane::tick(uint64_t now_ms) noexcept {
    for (int ch = 0; ch < kMaxChannels; ++ch) {
        if (splitters_[ch].has_partial() &&
            now_ms - last_rx_ms_[ch] >= cfg::CTRL_FRAME_TIMEOUT_MS) {
            splitters_[ch].clear_partial();
            respond_(ch, Err::FRAME_TIMEOUT, "control frame reassembly timed out");
        }
    }
}

void ControlPlane::on_frame_(int ch, const char* frame, size_t len) noexcept {
    // 复制到暂存（Router 会就地改写输入缓冲）
    std::memcpy(frame_buf_, frame, len);
    size_t resp_len = 0;
    const Err code = router_->handle(frame_buf_, len, resp_buf_, sizeof(resp_buf_),
                                     &resp_len);
    if (resp_len != 0 && sinks_[ch].send != nullptr) {
        const Err se = sinks_[ch].send(sinks_[ch].user,
                                       reinterpret_cast<const uint8_t*>(resp_buf_),
                                       resp_len);
        (void)se;  // 发送失败由传输层记录（协议不重传控制响应）
    }
    (void)code;  // 日志层使用（平台层自行记录）
}

void ControlPlane::respond_(int ch, Err code, const char* message) noexcept {
    size_t resp_len = 0;
    if (CommandRouter::respond_error(resp_buf_, sizeof(resp_buf_), &resp_len, code,
                                     message) == Err::INTERNAL_ERROR) {
        resp_len = 0;
    }
    if (resp_len != 0 && sinks_[ch].send != nullptr) {
        (void)sinks_[ch].send(sinks_[ch].user,
                              reinterpret_cast<const uint8_t*>(resp_buf_), resp_len);
    }
}

}  // namespace ijpd
