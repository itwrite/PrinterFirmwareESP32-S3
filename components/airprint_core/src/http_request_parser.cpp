// http_request_parser.cpp - Bounded incremental HTTP/1.1 request parser.
#include "airprint_core/http_request_parser.h"

#include <cstring>

namespace ijpd {
namespace airprint {
namespace {

bool is_space(uint8_t value) noexcept { return value == ' ' || value == '\t'; }

bool is_digit(uint8_t value) noexcept { return value >= '0' && value <= '9'; }

int hex_digit(uint8_t value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

}  // namespace

const char* http_parser_error_name(HttpParserError error) noexcept {
    switch (error) {
        case HttpParserError::Ok: return "ok";
        case HttpParserError::NeedMore: return "need_more";
        case HttpParserError::InvalidArgument: return "invalid_argument";
        case HttpParserError::InvalidState: return "invalid_state";
        case HttpParserError::HeaderTooLarge: return "header_too_large";
        case HttpParserError::HeaderLineTooLarge: return "header_line_too_large";
        case HttpParserError::TooManyHeaders: return "too_many_headers";
        case HttpParserError::InvalidRequestLine: return "invalid_request_line";
        case HttpParserError::InvalidHeader: return "invalid_header";
        case HttpParserError::InvalidContentLength: return "invalid_content_length";
        case HttpParserError::ConflictingContentLength: return "conflicting_content_length";
        case HttpParserError::UnsupportedTransferEncoding:
            return "unsupported_transfer_encoding";
        case HttpParserError::BodyTooLarge: return "body_too_large";
    }
    return "unknown";
}

HttpRequestParser::HttpRequestParser(const HttpParserLimits& limits) noexcept
    : limits_(limits) {
    if (limits_.max_header_bytes == 0 || limits_.max_header_bytes > kHttpMaxHeaderBytes) {
        limits_.max_header_bytes = kHttpMaxHeaderBytes;
    }
    if (limits_.max_header_line_bytes == 0 ||
        limits_.max_header_line_bytes > kHttpMaxHeaderLineBytes) {
        limits_.max_header_line_bytes = kHttpMaxHeaderLineBytes;
    }
    if (limits_.max_headers == 0 || limits_.max_headers > kHttpMaxHeaders) {
        limits_.max_headers = kHttpMaxHeaders;
    }
}

void HttpRequestParser::reset() noexcept {
    state_ = State::Headers;
    complete_ = false;
    failed_ = false;
    header_length_ = 0;
    chunk_line_length_ = 0;
    chunk_crlf_bytes_ = 0;
    chunk_remaining_ = 0;
    chunk_trailer_bytes_ = 0;
    head_ = HttpRequestHead{};
    body_received_ = 0;
    for (HttpHeaderView& header : headers_) header = HttpHeaderView{};
}

HttpFeedResult HttpRequestParser::error_(HttpParserError error,
                                         size_t consumed) noexcept {
    state_ = State::Failed;
    failed_ = true;
    return HttpFeedResult{HttpFeedEvent::Error, error, consumed, nullptr, 0, false};
}

HttpFeedResult HttpRequestParser::feed(const uint8_t* data, size_t length) noexcept {
    if (data == nullptr && length != 0) return error_(HttpParserError::InvalidArgument, 0);
    if (failed_ || complete_) return error_(HttpParserError::InvalidState, 0);
    if (length == 0) {
        return HttpFeedResult{HttpFeedEvent::NeedMore, HttpParserError::NeedMore,
                              0, nullptr, 0, false};
    }

    if (state_ == State::Headers) {
        size_t consumed = 0;
        for (; consumed < length; ++consumed) {
            if (header_length_ >= limits_.max_header_bytes) {
                return error_(HttpParserError::HeaderTooLarge, consumed);
            }
            header_buffer_[header_length_++] = data[consumed];
            if (header_length_ >= 4 &&
                header_buffer_[header_length_ - 4] == '\r' &&
                header_buffer_[header_length_ - 3] == '\n' &&
                header_buffer_[header_length_ - 2] == '\r' &&
                header_buffer_[header_length_ - 1] == '\n') {
                const HttpFeedResult parsed = parse_headers_(header_length_);
                if (parsed.event == HttpFeedEvent::Error) return parsed;
                if (head_.chunked) {
                    state_ = State::ChunkSize;
                } else {
                    state_ = head_.content_length == 0 ? State::Complete : State::Body;
                }
                complete_ = head_.content_length == 0;
                if (head_.chunked) complete_ = false;
                HttpFeedResult result = parsed;
                result.consumed = consumed + 1;
                result.message_complete = complete_;
                if (complete_) result.event = HttpFeedEvent::HeadersComplete;
                return result;
            }
        }
        return HttpFeedResult{HttpFeedEvent::NeedMore, HttpParserError::NeedMore,
                              consumed, nullptr, 0, false};
    }

    if (state_ == State::ChunkSize || state_ == State::ChunkSizeLf ||
        state_ == State::ChunkData || state_ == State::ChunkDataCrlf ||
        state_ == State::ChunkTrailers || state_ == State::ChunkTrailerLf) {
        return feed_chunked_(data, length);
    }
    if (state_ != State::Body) return error_(HttpParserError::InvalidState, 0);
    const uint64_t remaining = head_.content_length - body_received_;
    const size_t take = remaining < length ? static_cast<size_t>(remaining) : length;
    body_received_ += take;
    const bool complete = body_received_ == head_.content_length;
    if (complete) {
        state_ = State::Complete;
        complete_ = true;
    }
    return HttpFeedResult{HttpFeedEvent::BodyChunk, HttpParserError::Ok,
                          take, data, take, complete};
}

const HttpHeaderView* HttpRequestParser::header(size_t index) const noexcept {
    return index < head_.header_count ? &headers_[index] : nullptr;
}

const HttpHeaderView* HttpRequestParser::find_header(const char* name) const noexcept {
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < head_.header_count; ++i) {
        if (equals_ci_(headers_[i].name, name)) return &headers_[i];
    }
    return nullptr;
}

HttpFeedResult HttpRequestParser::parse_headers_(size_t header_length) noexcept {
    size_t request_line_end = 0;
    if (!find_crlf_(0, header_length, &request_line_end)) {
        return error_(HttpParserError::InvalidRequestLine, 0);
    }
    HttpParserError error = parse_request_line_(0, request_line_end);
    if (error != HttpParserError::Ok) return error_(error, 0);
    error = parse_header_lines_(request_line_end + 2, header_length - 2);
    if (error != HttpParserError::Ok) return error_(error, 0);
    error = finalize_headers_();
    if (error != HttpParserError::Ok) return error_(error, 0);
    HttpFeedResult result{HttpFeedEvent::HeadersComplete, HttpParserError::Ok,
                          0, nullptr, 0, false};
    return result;
}

HttpFeedResult HttpRequestParser::feed_chunked_(const uint8_t* data,
                                                size_t length) noexcept {
    size_t offset = 0;
    while (offset < length) {
        if (state_ == State::ChunkSize || state_ == State::ChunkTrailers) {
            const bool trailers = state_ == State::ChunkTrailers;
            const uint8_t value = data[offset++];
            if (value == '\r') {
                state_ = trailers ? State::ChunkTrailerLf : State::ChunkSizeLf;
                continue;
            }
            if (value == '\n' || chunk_line_length_ >= limits_.max_header_line_bytes) {
                return error_(trailers ? HttpParserError::InvalidHeader
                                       : HttpParserError::InvalidContentLength,
                              offset);
            }
            chunk_line_buffer_[chunk_line_length_++] = value;
            if (++chunk_trailer_bytes_ > limits_.max_header_bytes && trailers) {
                return error_(HttpParserError::HeaderTooLarge, offset);
            }
            continue;
        }

        if (state_ == State::ChunkSizeLf) {
            if (data[offset++] != '\n') {
                return error_(HttpParserError::InvalidContentLength, offset);
            }
            const HttpParserError parsed = parse_chunk_size_();
            if (parsed != HttpParserError::Ok) return error_(parsed, offset);
            chunk_line_length_ = 0;
            chunk_trailer_bytes_ = 0;
            if (chunk_remaining_ == 0) {
                state_ = State::ChunkTrailers;
            } else {
                state_ = State::ChunkData;
            }
            continue;
        }

        if (state_ == State::ChunkData) {
            const uint64_t available = static_cast<uint64_t>(length - offset);
            const size_t take = static_cast<size_t>(
                chunk_remaining_ < available ? chunk_remaining_ : available);
            if (take == 0) continue;
            const uint8_t* body = data + offset;
            offset += take;
            chunk_remaining_ -= take;
            body_received_ += take;
            if (chunk_remaining_ == 0) {
                state_ = State::ChunkDataCrlf;
                chunk_crlf_bytes_ = 0;
            }
            return HttpFeedResult{HttpFeedEvent::BodyChunk, HttpParserError::Ok,
                                  offset, body, take, false};
        }

        if (state_ == State::ChunkDataCrlf) {
            const uint8_t expected = chunk_crlf_bytes_ == 0 ? '\r' : '\n';
            if (data[offset++] != expected) {
                return error_(HttpParserError::InvalidHeader, offset);
            }
            ++chunk_crlf_bytes_;
            if (chunk_crlf_bytes_ == 2) {
                chunk_crlf_bytes_ = 0;
                state_ = State::ChunkSize;
                chunk_line_length_ = 0;
            }
            continue;
        }

        if (state_ == State::ChunkTrailerLf) {
            if (data[offset++] != '\n') {
                return error_(HttpParserError::InvalidHeader, offset);
            }
            const bool last_trailer = chunk_line_length_ == 0;
            chunk_line_length_ = 0;
            chunk_trailer_bytes_ = 0;
            if (last_trailer) {
                state_ = State::Complete;
                complete_ = true;
                return HttpFeedResult{HttpFeedEvent::BodyChunk, HttpParserError::Ok,
                                      offset, nullptr, 0, true};
            }
            state_ = State::ChunkTrailers;
            continue;
        }
    }
    return HttpFeedResult{HttpFeedEvent::NeedMore, HttpParserError::NeedMore,
                          offset, nullptr, 0, false};
}

HttpParserError HttpRequestParser::parse_chunk_size_() noexcept {
    size_t end = 0;
    while (end < chunk_line_length_ && chunk_line_buffer_[end] != ';') ++end;
    if (end == 0) return HttpParserError::InvalidContentLength;
    uint64_t value = 0;
    for (size_t i = 0; i < end; ++i) {
        const int digit = hex_digit(chunk_line_buffer_[i]);
        if (digit < 0 || value > (UINT64_MAX - static_cast<uint64_t>(digit)) / 16) {
            return HttpParserError::InvalidContentLength;
        }
        value = value * 16 + static_cast<uint64_t>(digit);
    }
    if (value > limits_.max_body_bytes - body_received_) {
        return HttpParserError::BodyTooLarge;
    }
    chunk_remaining_ = value;
    return HttpParserError::Ok;
}

HttpParserError HttpRequestParser::parse_request_line_(size_t begin, size_t end) noexcept {
    size_t first_space = begin;
    while (first_space < end && header_buffer_[first_space] != ' ') ++first_space;
    if (first_space == begin || first_space >= end) return HttpParserError::InvalidRequestLine;
    size_t second_space = first_space + 1;
    while (second_space < end && header_buffer_[second_space] != ' ') ++second_space;
    if (second_space == first_space + 1 || second_space >= end) {
        return HttpParserError::InvalidRequestLine;
    }
    const size_t version_begin = second_space + 1;
    const size_t version_length = end - version_begin;
    if (version_length != 8 ||
        std::memcmp(header_buffer_ + version_begin, "HTTP/1.", 7) != 0 ||
        (header_buffer_[version_begin + 7] != '0' &&
         header_buffer_[version_begin + 7] != '1')) {
        return HttpParserError::InvalidRequestLine;
    }
    for (size_t i = begin; i < first_space; ++i) {
        if (!is_token_char_(header_buffer_[i])) return HttpParserError::InvalidRequestLine;
    }
    if (first_space - begin > UINT16_MAX || second_space - first_space - 1 > UINT16_MAX) {
        return HttpParserError::InvalidRequestLine;
    }
    head_.method = {reinterpret_cast<const char*>(header_buffer_ + begin),
                    static_cast<uint16_t>(first_space - begin)};
    head_.target = {reinterpret_cast<const char*>(header_buffer_ + first_space + 1),
                    static_cast<uint16_t>(second_space - first_space - 1)};
    head_.version_major = 1;
    head_.version_minor = static_cast<uint8_t>(header_buffer_[version_begin + 7] - '0');
    head_.keep_alive = head_.version_minor == 1;
    return HttpParserError::Ok;
}

HttpParserError HttpRequestParser::parse_header_lines_(size_t begin, size_t end) noexcept {
    size_t cursor = begin;
    const size_t body_end = end >= 2 && header_buffer_[end - 2] == '\r' &&
                                    header_buffer_[end - 1] == '\n'
                                ? end - 2
                                : end;
    while (cursor < body_end) {
        size_t line_end = 0;
        if (!find_crlf_(cursor, body_end + 2, &line_end)) return HttpParserError::InvalidHeader;
        if (line_end == cursor) break;
        if (line_end - cursor > limits_.max_header_line_bytes) {
            return HttpParserError::HeaderLineTooLarge;
        }
        const HttpParserError error = parse_header_line_(cursor, line_end);
        if (error != HttpParserError::Ok) return error;
        cursor = line_end + 2;
    }
    return HttpParserError::Ok;
}

HttpParserError HttpRequestParser::parse_header_line_(size_t begin, size_t end) noexcept {
    size_t colon = begin;
    while (colon < end && header_buffer_[colon] != ':') ++colon;
    if (colon == begin || colon >= end) return HttpParserError::InvalidHeader;
    for (size_t i = begin; i < colon; ++i) {
        if (!is_token_char_(header_buffer_[i])) return HttpParserError::InvalidHeader;
    }
    size_t value_begin = colon + 1;
    while (value_begin < end && is_space(header_buffer_[value_begin])) ++value_begin;
    size_t value_end = end;
    while (value_end > value_begin && is_space(header_buffer_[value_end - 1])) --value_end;
    if (head_.header_count >= limits_.max_headers) return HttpParserError::TooManyHeaders;
    headers_[head_.header_count++] = {
        {reinterpret_cast<const char*>(header_buffer_ + begin),
         static_cast<uint16_t>(colon - begin)},
        {reinterpret_cast<const char*>(header_buffer_ + value_begin),
         static_cast<uint16_t>(value_end - value_begin)},
    };
    return HttpParserError::Ok;
}

HttpParserError HttpRequestParser::finalize_headers_() noexcept {
    const HttpHeaderView* content_length = find_header("Content-Length");
    if (content_length != nullptr) {
        uint64_t value = 0;
        if (!parse_decimal_(content_length->value, &value)) {
            return HttpParserError::InvalidContentLength;
        }
        for (size_t i = 0; i < head_.header_count; ++i) {
            if (equals_ci_(headers_[i].name, "Content-Length") &&
                (headers_[i].value.data != content_length->value.data ||
                 headers_[i].value.length != content_length->value.length)) {
                uint64_t duplicate = 0;
                if (!parse_decimal_(headers_[i].value, &duplicate) || duplicate != value) {
                    return HttpParserError::ConflictingContentLength;
                }
            }
        }
        if (value > limits_.max_body_bytes) return HttpParserError::BodyTooLarge;
        head_.has_content_length = true;
        head_.content_length = value;
    }
    const HttpHeaderView* transfer_encoding = find_header("Transfer-Encoding");
    if (transfer_encoding != nullptr) {
        if (!equals_ci_(transfer_encoding->value, "chunked") || head_.has_content_length) {
            return HttpParserError::UnsupportedTransferEncoding;
        }
        head_.chunked = true;
    }

    const HttpHeaderView* content_type = find_header("Content-Type");
    if (content_type != nullptr) head_.content_type = content_type->value;
    const HttpHeaderView* expect = find_header("Expect");
    if (expect != nullptr) {
        if (!equals_ci_(expect->value, "100-continue")) return HttpParserError::InvalidHeader;
        head_.expect_continue = true;
    }
    const HttpHeaderView* connection = find_header("Connection");
    if (connection != nullptr) {
        bool keep_alive = head_.keep_alive;
        if (!parse_connection_(connection->value, &keep_alive)) {
            return HttpParserError::InvalidHeader;
        }
        head_.keep_alive = keep_alive;
    }
    return HttpParserError::Ok;
}

bool HttpRequestParser::find_crlf_(size_t begin, size_t end, size_t* line_end) const noexcept {
    if (line_end == nullptr || end > header_length_ || begin > end) return false;
    for (size_t i = begin; i + 1 < end; ++i) {
        if (header_buffer_[i] == '\r' && header_buffer_[i + 1] == '\n') {
            *line_end = i;
            return true;
        }
    }
    return false;
}

bool HttpRequestParser::is_token_char_(uint8_t value) const noexcept {
    if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9')) return true;
    switch (value) {
        case '!': case '#': case '$': case '%': case '&': case '\'' : case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool HttpRequestParser::equals_ci_(HttpStringView value, const char* literal) const noexcept {
    if (literal == nullptr) return false;
    const size_t length = std::strlen(literal);
    if (length != value.length) return false;
    for (size_t i = 0; i < length; ++i) {
        char a = value.data[i];
        char b = literal[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

HttpStringView HttpRequestParser::trim_(size_t begin, size_t end) const noexcept {
    while (begin < end && is_space(header_buffer_[begin])) ++begin;
    while (end > begin && is_space(header_buffer_[end - 1])) --end;
    return {reinterpret_cast<const char*>(header_buffer_ + begin),
            static_cast<uint16_t>(end - begin)};
}

bool HttpRequestParser::parse_decimal_(HttpStringView value, uint64_t* out) const noexcept {
    if (out == nullptr || value.data == nullptr || value.length == 0) return false;
    uint64_t result = 0;
    for (size_t i = 0; i < value.length; ++i) {
        const uint8_t digit = static_cast<uint8_t>(value.data[i]);
        if (!is_digit(digit)) return false;
        if (result > (UINT64_MAX - (digit - '0')) / 10) return false;
        result = result * 10 + (digit - '0');
    }
    *out = result;
    return true;
}

bool HttpRequestParser::parse_connection_(HttpStringView value, bool* keep_alive) const noexcept {
    if (keep_alive == nullptr) return false;
    if (equals_ci_(value, "close")) {
        *keep_alive = false;
        return true;
    }
    if (equals_ci_(value, "keep-alive")) {
        *keep_alive = true;
        return true;
    }
    return false;
}

}  // namespace airprint
}  // namespace ijpd
