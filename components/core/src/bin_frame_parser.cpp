// bin_frame_parser.cpp —— 数据通道帧状态机
#include "core/bin_frame_parser.h"

#include <cstring>

#include "core/byte_order.h"

namespace ijpd {
namespace {

constexpr uint8_t kMagic[4] = {0x49, 0x4A, 0x50, 0x44};  // "IJPD"
constexpr uint16_t kHelloVersion = cfg::PROTOCOL_HELLO_VERSION;

}  // namespace

Err BinFrameParser::begin_job(const JobConfig& cfg) noexcept {
    if (cfg.total_packets < 1 || cfg.total_packets > cfg::MAX_TOTAL_PACKETS) {
        return Err::INVALID_ARGUMENT;
    }
    if (cfg.nominal_packet_bytes < 1 || cfg.last_packet_bytes < 1 ||
        cfg.last_packet_bytes > cfg.nominal_packet_bytes) {
        return Err::INVALID_ARGUMENT;
    }
    const uint64_t expect_total =
        static_cast<uint64_t>(cfg.nominal_packet_bytes) * (cfg.total_packets - 1) +
        cfg.last_packet_bytes;
    if (expect_total != cfg.total_bytes_uncompressed) {
        return Err::INVALID_ARGUMENT;
    }
    if (cfg.compression != Compression::None && cfg.compression != Compression::Lz4Block) {
        return Err::INVALID_ARGUMENT;
    }
    if (cfg.integrity != Integrity::None && cfg.integrity != Integrity::Crc32) {
        return Err::INVALID_ARGUMENT;
    }

    cfg_ = cfg;
    phase_ = Phase::WaitHello;
    last_error_ = Err::OK;
    hello_filled_ = 0;
    hdr_filled_ = 0;
    payload_filled_ = 0;
    crc_filled_ = 0;
    buf_ = nullptr;
    buf_cap_ = 0;
    packets_ = 0;
    bytes_ = 0;
    return Err::OK;
}

void BinFrameParser::reset() noexcept {
    phase_ = Phase::Idle;
    last_error_ = Err::OK;
    hello_filled_ = 0;
    hdr_filled_ = 0;
    payload_filled_ = 0;
    crc_filled_ = 0;
    buf_ = nullptr;
    buf_cap_ = 0;
    packets_ = 0;
    bytes_ = 0;
}

Err BinFrameParser::set_payload_buffer(uint8_t* buf, size_t cap) noexcept {
    if (buf == nullptr || cap < cfg::MAX_PACKET_COMPRESSED_SIZE) {
        return Err::INVALID_ARGUMENT;
    }
    buf_ = buf;
    buf_cap_ = cap;
    return Err::OK;
}

BinFrameParser::FeedResult BinFrameParser::fail_(Err e) noexcept {
    phase_ = Phase::Failed;
    last_error_ = e;
    FeedResult r;
    r.status = FeedStatus::Failed;
    r.error = e;
    return r;
}

BinFrameParser::FeedResult BinFrameParser::ok_(size_t consumed, uint32_t frames) noexcept {
    FeedResult r;
    r.status = FeedStatus::Ok;
    r.error = Err::OK;
    r.consumed = consumed;
    r.frames = frames;
    return r;
}

Err BinFrameParser::validate_header_(uint16_t idx, uint16_t ulen,
                                      uint16_t clen) const noexcept {
    // 1. 严格顺序
    if (idx != packets_) return Err::PACKET_OUT_OF_ORDER;
    // 2. 非零长度（u16 字段上限即 MAX，无需再查上界）
    if (ulen == 0) return Err::PACKET_LENGTH_INVALID;
    if (clen == 0) return Err::PACKET_LENGTH_INVALID;
    // 3. 压缩模式长度关系（含存储兜底语义）
    if (cfg_.compression == Compression::None && clen != ulen) {
        return Err::PACKET_LENGTH_INVALID;
    }
    if (cfg_.compression == Compression::Lz4Block && clen > ulen) {
        return Err::PACKET_LENGTH_INVALID;
    }
    // 4. 标称包长 / 末包长度
    const uint16_t want =
        (idx == cfg_.total_packets - 1) ? cfg_.last_packet_bytes
                                        : cfg_.nominal_packet_bytes;
    if (ulen != want) return Err::PACKET_LENGTH_INVALID;
    return Err::OK;
}

BinFrameParser::FeedResult BinFrameParser::feed(const uint8_t* data, size_t len,
                                                const Events& ev) noexcept {
    if (phase_ == Phase::Idle) {
        return fail_(Err::NO_ACTIVE_JOB);
    }
    if (phase_ == Phase::Failed) {
        FeedResult r;
        r.status = FeedStatus::Failed;
        r.error = last_error_;
        r.consumed = len;  // 吸收丢弃
        return r;
    }
    if (data == nullptr && len != 0) {
        return fail_(Err::INVALID_ARGUMENT);
    }

    size_t i = 0;
    uint32_t frames = 0;

    // ---- Hello（12B，仅一次）----
    if (phase_ == Phase::WaitHello) {
        while (hello_filled_ < kHelloSize && i < len) {
            hello_[hello_filled_++] = data[i++];
        }
        if (hello_filled_ < kHelloSize) return ok_(i, frames);

        if (std::memcmp(hello_, kMagic, 4) != 0 ||
            read_u16_le(hello_ + 4) != kHelloVersion ||
            read_u32_le(hello_ + 6) != cfg_.data_token ||
            read_u16_le(hello_ + 10) != 0) {
            return fail_(Err::DATA_HANDSHAKE_FAILED);
        }
        phase_ = Phase::Header;
        hdr_filled_ = 0;
        if (ev.on_hello_ok != nullptr) ev.on_hello_ok(ev.user);
    }

    // ---- 帧循环 ----
    for (;;) {
        if (phase_ == Phase::Header) {
            while (hdr_filled_ < 6 && i < len) {
                hdr_[hdr_filled_++] = data[i++];
            }
            if (hdr_filled_ < 6) return ok_(i, frames);

            idx_ = read_u16_le(hdr_);
            ulen_ = read_u16_le(hdr_ + 2);
            clen_ = read_u16_le(hdr_ + 4);
            const Err ve = validate_header_(idx_, ulen_, clen_);
            if (ve != Err::OK) return fail_(ve);

            crc_state_ = crc32_update(CRC32_INIT_STATE, hdr_, 6);
            payload_filled_ = 0;
            phase_ = Phase::Payload;
        }

        if (phase_ == Phase::Payload) {
            if (buf_ == nullptr) {
                if (i < len) {
                    // 载荷字节待存但缓冲未就绪：报告 NeedBuffer，头已消费
                    FeedResult r;
                    r.status = FeedStatus::NeedBuffer;
                    r.error = Err::OK;
                    r.consumed = i;
                    r.frames = frames;
                    return r;
                }
                return ok_(i, frames);  // 本块恰好用尽，等下一块再要缓冲
            }
            if (clen_ > buf_cap_) return fail_(Err::INTERNAL_ERROR);  // 设置时已校验，防御
            const size_t want = clen_ - payload_filled_;
            const size_t take = (want < len - i) ? want : (len - i);
            if (take != 0) {
                std::memcpy(buf_ + payload_filled_, data + i, take);
                crc_state_ = crc32_update(crc_state_, data + i, take);
                payload_filled_ += take;
                i += take;
            }
            if (payload_filled_ < clen_) return ok_(i, frames);
            phase_ = (cfg_.integrity == Integrity::Crc32) ? Phase::Crc : Phase::Done;
            if (phase_ == Phase::Crc) {
                crc_filled_ = 0;
                continue;
            }
        }

        if (phase_ == Phase::Crc) {
            while (crc_filled_ < 4 && i < len) {
                crc_[crc_filled_++] = data[i++];
            }
            if (crc_filled_ < 4) return ok_(i, frames);
            if (crc32_final(crc_state_) != read_u32_le(crc_)) {
                return fail_(Err::PACKET_CRC_MISMATCH);
            }
        }

        // ---- Dispatch（Header/Payload/Crc 全部通过；Done 为“帧就绪”标记）----
        {
            packets_ += 1;
            bytes_ += ulen_;
            if (ev.on_frame != nullptr) {
                Frame f{idx_, ulen_, clen_, buf_};
                ev.on_frame(ev.user, f);
            }
            ++frames;
            buf_ = nullptr;  // 强制显式重设，防止未消费数据被覆盖
            hdr_filled_ = 0;
            crc_filled_ = 0;
            if (packets_ >= cfg_.total_packets) {
                phase_ = Phase::Done;
                if (i < len) {
                    return fail_(Err::PACKET_LENGTH_INVALID);  // 末包后多余字节
                }
                return ok_(i, frames);
            }
            phase_ = Phase::Header;
            continue;
        }
    }
}

}  // namespace ijpd
