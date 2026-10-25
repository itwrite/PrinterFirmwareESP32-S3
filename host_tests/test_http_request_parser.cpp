// test_http_request_parser.cpp - Incremental HTTP header/body boundary tests.
#include "assert.h"

#include <cstring>

#include "airprint_core/http_request_parser.h"

using namespace ijpd::airprint;

namespace {

const char* view_string(HttpStringView view, char* buffer, size_t capacity) {
    if (view.length + 1 > capacity) return nullptr;
    std::memcpy(buffer, view.data, view.length);
    buffer[view.length] = '\0';
    return buffer;
}

}  // namespace

int main() {
    const char request[] =
        "POST /ipp/print HTTP/1.1\r\n"
        "Host: printer.local\r\n"
        "Content-Type: application/ipp\r\n"
        "Content-Length: 5\r\n"
        "Expect: 100-continue\r\n"
        "Connection: close\r\n"
        "\r\n"
        "abcde";
    HttpRequestParser parser;

    HttpFeedResult result = parser.feed(reinterpret_cast<const uint8_t*>(request), 12);
    CHECK_TRUE(result.event == HttpFeedEvent::NeedMore);
    result = parser.feed(reinterpret_cast<const uint8_t*>(request) + 12,
                         sizeof(request) - 1 - 12);
    CHECK_TRUE(result.event == HttpFeedEvent::HeadersComplete);
    CHECK_EQ(result.consumed, sizeof(request) - 1 - 12 - 5u);
    CHECK_TRUE(!result.message_complete);
    CHECK_EQ(parser.head().content_length, 5u);
    CHECK_TRUE(parser.head().has_content_length);
    CHECK_TRUE(parser.head().expect_continue);
    CHECK_TRUE(!parser.head().keep_alive);

    char method[16] = {};
    char target[64] = {};
    CHECK_EQ_STR(view_string(parser.head().method, method, sizeof(method)), "POST");
    CHECK_EQ_STR(view_string(parser.head().target, target, sizeof(target)), "/ipp/print");
    const HttpHeaderView* content_type = parser.find_header("content-type");
    CHECK_TRUE(content_type != nullptr);
    char type[64] = {};
    CHECK_EQ_STR(view_string(content_type->value, type, sizeof(type)), "application/ipp");

    // Feed the body in two chunks; the second chunk completes exactly at EOF.
    result = parser.feed(reinterpret_cast<const uint8_t*>("ab"), 2);
    CHECK_TRUE(result.event == HttpFeedEvent::BodyChunk && !result.message_complete);
    CHECK_EQ(result.consumed, 2u);
    CHECK_EQ_MEM(result.body, "ab", 2);
    result = parser.feed(reinterpret_cast<const uint8_t*>("cde"), 3);
    CHECK_TRUE(result.event == HttpFeedEvent::BodyChunk && result.message_complete);
    CHECK_EQ(result.consumed, 3u);
    CHECK_EQ_MEM(result.body, "cde", 3);
    CHECK_TRUE(parser.complete());

    // A second request can be parsed by resetting the connection parser.
    parser.reset();
    const char keep_alive[] =
        "POST /ipp/print HTTP/1.1\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive\r\n\r\n";
    result = parser.feed(reinterpret_cast<const uint8_t*>(keep_alive), sizeof(keep_alive) - 1);
    CHECK_TRUE(result.event == HttpFeedEvent::HeadersComplete && result.message_complete);
    CHECK_TRUE(parser.head().keep_alive);

    // Header and body in one TCP read: body is intentionally not consumed by
    // the header event, so the caller can pass the remaining bytes to feed().
    parser.reset();
    const char one_read[] =
        "POST /ipp/print HTTP/1.1\r\nContent-Length: 4\r\n\r\nWXYZ";
    result = parser.feed(reinterpret_cast<const uint8_t*>(one_read), sizeof(one_read) - 1);
    CHECK_TRUE(result.event == HttpFeedEvent::HeadersComplete);
    const size_t body_offset = result.consumed;
    result = parser.feed(reinterpret_cast<const uint8_t*>(one_read) + body_offset,
                         sizeof(one_read) - 1 - body_offset);
    CHECK_TRUE(result.event == HttpFeedEvent::BodyChunk && result.message_complete);
    CHECK_EQ_MEM(result.body, "WXYZ", 4);

    // Header errors and limits.
    parser.reset();
    const char bad_request[] = "POST / HTTP/2.0\r\n\r\n";
    result = parser.feed(reinterpret_cast<const uint8_t*>(bad_request),
                         sizeof(bad_request) - 1);
    CHECK_TRUE(result.event == HttpFeedEvent::Error &&
               result.error == HttpParserError::InvalidRequestLine);

    parser.reset();
    const char chunked[] =
        "POST /ipp/print HTTP/1.1\r\n"
        "Content-Type: application/ipp\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n"
        "5\r\npedia\r\n"
        "0\r\n\r\n";
    result = parser.feed(reinterpret_cast<const uint8_t*>(chunked), sizeof(chunked) - 1);
    CHECK_TRUE(result.event == HttpFeedEvent::HeadersComplete);
    const size_t chunk_body_offset = result.consumed;
    const uint8_t* chunk_cursor = reinterpret_cast<const uint8_t*>(chunked) +
                                  chunk_body_offset;
    size_t chunk_remaining = sizeof(chunked) - 1 - chunk_body_offset;
    bool saw_wiki = false;
    bool saw_pedia = false;
    while (chunk_remaining != 0 && !parser.complete()) {
        result = parser.feed(chunk_cursor, chunk_remaining);
        CHECK_TRUE(result.event == HttpFeedEvent::BodyChunk ||
                   result.event == HttpFeedEvent::NeedMore);
        if (result.event == HttpFeedEvent::BodyChunk && result.body_length != 0) {
            if (result.body_length == 4) {
                saw_wiki = std::memcmp(result.body, "Wiki", 4) == 0;
            } else if (result.body_length == 5) {
                saw_pedia = std::memcmp(result.body, "pedia", 5) == 0;
            }
        }
        CHECK_TRUE(result.consumed != 0 || result.message_complete);
        chunk_cursor += result.consumed;
        chunk_remaining -= result.consumed;
    }
    CHECK_TRUE(saw_wiki);
    CHECK_TRUE(saw_pedia);
    CHECK_TRUE(parser.complete());

    parser.reset();
    const char chunked_with_length[] =
        "POST /ipp/print HTTP/1.1\r\n"
        "Content-Length: 4\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    result = parser.feed(reinterpret_cast<const uint8_t*>(chunked_with_length),
                         sizeof(chunked_with_length) - 1);
    CHECK_TRUE(result.event == HttpFeedEvent::Error &&
               result.error == HttpParserError::UnsupportedTransferEncoding);

    parser.reset();
    const char conflicting[] =
        "POST /ipp/print HTTP/1.1\r\nContent-Length: 2\r\n"
        "Content-Length: 3\r\n\r\n";
    result = parser.feed(reinterpret_cast<const uint8_t*>(conflicting),
                         sizeof(conflicting) - 1);
    CHECK_TRUE(result.event == HttpFeedEvent::Error &&
               result.error == HttpParserError::ConflictingContentLength);

    HttpParserLimits small_limits;
    small_limits.max_body_bytes = 2;
    HttpRequestParser small_parser(small_limits);
    const char too_large[] =
        "POST /ipp/print HTTP/1.1\r\nContent-Length: 3\r\n\r\n";
    result = small_parser.feed(reinterpret_cast<const uint8_t*>(too_large),
                               sizeof(too_large) - 1);
    CHECK_TRUE(result.event == HttpFeedEvent::Error &&
               result.error == HttpParserError::BodyTooLarge);

    return ::ijpd::test::summary("test_http_request_parser");
}
