// ============================================================================
// http_request_parser.h - Bounded incremental HTTP/1.1 request parser.
//
// The parser owns only the request header bytes. Body bytes are returned as a
// zero-copy view into the caller's feed buffer and are never accumulated.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_HTTP_REQUEST_PARSER_H
#define IJPD_AIRPRINT_CORE_HTTP_REQUEST_PARSER_H

#include <cstddef>
#include <cstdint>

namespace ijpd {
namespace airprint {

inline constexpr size_t kHttpMaxHeaderBytes = 8192;
inline constexpr size_t kHttpMaxHeaderLineBytes = 1024;
inline constexpr size_t kHttpMaxHeaders = 32;

struct HttpStringView {
    const char* data = nullptr;
    uint16_t length = 0;

    bool empty() const noexcept { return length == 0; }
};

struct HttpHeaderView {
    HttpStringView name{};
    HttpStringView value{};
};

struct HttpParserLimits {
    size_t max_header_bytes = kHttpMaxHeaderBytes;
    size_t max_header_line_bytes = kHttpMaxHeaderLineBytes;
    size_t max_headers = kHttpMaxHeaders;
    uint64_t max_body_bytes = 0xFFFFFFFFu;
};

enum class HttpParserError : uint8_t {
    Ok = 0,
    NeedMore,
    InvalidArgument,
    InvalidState,
    HeaderTooLarge,
    HeaderLineTooLarge,
    TooManyHeaders,
    InvalidRequestLine,
    InvalidHeader,
    InvalidContentLength,
    ConflictingContentLength,
    UnsupportedTransferEncoding,
    BodyTooLarge,
};

const char* http_parser_error_name(HttpParserError error) noexcept;

enum class HttpFeedEvent : uint8_t {
    NeedMore = 0,
    HeadersComplete,
    BodyChunk,
    Error,
};

struct HttpFeedResult {
    HttpFeedEvent event = HttpFeedEvent::NeedMore;
    HttpParserError error = HttpParserError::Ok;
    size_t consumed = 0;
    const uint8_t* body = nullptr;
    size_t body_length = 0;
    bool message_complete = false;
};

struct HttpRequestHead {
    HttpStringView method{};
    HttpStringView target{};
    uint8_t version_major = 0;
    uint8_t version_minor = 0;
    HttpStringView content_type{};
    uint64_t content_length = 0;
    bool has_content_length = false;
    bool chunked = false;
    bool expect_continue = false;
    bool keep_alive = false;
    size_t header_count = 0;
};

class HttpRequestParser {
public:
    explicit HttpRequestParser(const HttpParserLimits& limits = {}) noexcept;

    void reset() noexcept;
    HttpFeedResult feed(const uint8_t* data, size_t length) noexcept;

    const HttpRequestHead& head() const noexcept { return head_; }
    const HttpHeaderView* header(size_t index) const noexcept;
    const HttpHeaderView* find_header(const char* name) const noexcept;
    bool complete() const noexcept { return complete_; }
    bool failed() const noexcept { return failed_; }
    uint64_t body_received() const noexcept { return body_received_; }

private:
    enum class State : uint8_t {
        Headers,
        Body,
        ChunkSize,
        ChunkSizeLf,
        ChunkData,
        ChunkDataCrlf,
        ChunkTrailers,
        ChunkTrailerLf,
        Complete,
        Failed,
    };

    HttpFeedResult error_(HttpParserError error, size_t consumed) noexcept;
    HttpFeedResult parse_headers_(size_t header_length) noexcept;
    HttpFeedResult feed_chunked_(const uint8_t* data, size_t length) noexcept;
    HttpParserError parse_chunk_size_() noexcept;
    HttpParserError parse_request_line_(size_t begin, size_t end) noexcept;
    HttpParserError parse_header_lines_(size_t begin, size_t end) noexcept;
    HttpParserError parse_header_line_(size_t begin, size_t end) noexcept;
    HttpParserError finalize_headers_() noexcept;

    bool find_crlf_(size_t begin, size_t end, size_t* line_end) const noexcept;
    bool is_token_char_(uint8_t value) const noexcept;
    bool equals_ci_(HttpStringView value, const char* literal) const noexcept;
    HttpStringView trim_(size_t begin, size_t end) const noexcept;
    bool parse_decimal_(HttpStringView value, uint64_t* out) const noexcept;
    bool parse_connection_(HttpStringView value, bool* keep_alive) const noexcept;

    HttpParserLimits limits_{};
    State state_ = State::Headers;
    bool complete_ = false;
    bool failed_ = false;
    uint8_t header_buffer_[kHttpMaxHeaderBytes] = {};
    uint8_t chunk_line_buffer_[kHttpMaxHeaderLineBytes] = {};
    size_t header_length_ = 0;
    size_t chunk_line_length_ = 0;
    size_t chunk_crlf_bytes_ = 0;
    uint64_t chunk_remaining_ = 0;
    uint64_t chunk_trailer_bytes_ = 0;
    HttpHeaderView headers_[kHttpMaxHeaders] = {};
    HttpRequestHead head_{};
    uint64_t body_received_ = 0;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_HTTP_REQUEST_PARSER_H
