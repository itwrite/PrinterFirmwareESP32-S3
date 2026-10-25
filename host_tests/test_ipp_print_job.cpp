// test_ipp_print_job.cpp - Streaming Print-Job ingestion tests.
#include "assert.h"

#include <cstring>

#include "airprint_core/ipp_codec.h"
#include "airprint_core/ipp_job.h"
#include "airprint_core/ipp_print_job.h"

using namespace ijpd::airprint;

namespace {

struct BackendState {
    uint8_t bytes[128] = {};
    size_t length = 0;
    bool begin_called = false;
    bool end_success = false;
    bool cancel_called = false;
    bool fail_write = false;
    bool fail_cancel = false;
};

IppJobError backend_begin(void* user, const IppRequest&, const IppJobSnapshot&) {
    auto* state = static_cast<BackendState*>(user);
    state->begin_called = true;
    return IppJobError::Ok;
}

IppJobError backend_write(void* user, const uint8_t* data, size_t length) {
    auto* state = static_cast<BackendState*>(user);
    if (state->fail_write || data == nullptr || length > sizeof(state->bytes) - state->length) {
        return IppJobError::InvalidArgument;
    }
    std::memcpy(state->bytes + state->length, data, length);
    state->length += length;
    return IppJobError::Ok;
}

IppJobError backend_end(void* user, bool success) {
    auto* state = static_cast<BackendState*>(user);
    state->end_success = success;
    return IppJobError::Ok;
}

IppJobError backend_cancel(void* user) {
    auto* state = static_cast<BackendState*>(user);
    state->cancel_called = true;
    return state->fail_cancel ? IppJobError::InvalidArgument : IppJobError::Ok;
}

size_t make_job(uint8_t* buffer, size_t capacity, const char* body,
                const char* document_format = "image/urf") {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, IppOperation::PrintJob, 77) != IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.add_cstr(IppValueTag::Uri, "printer-uri",
                        "ipp://printer.local/ipp/print") != IppError::Ok ||
        writer.add_cstr(IppValueTag::MimeMediaType, "document-format", document_format) !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NameWithoutLanguage, "job-name", "photo") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NameWithoutLanguage,
                        "requesting-user-name", "alice") != IppError::Ok ||
        writer.end_attributes() != IppError::Ok) {
        return 0;
    }
    if (body != nullptr && writer.append_document(
                              reinterpret_cast<const uint8_t*>(body), std::strlen(body)) !=
            IppError::Ok) {
        return 0;
    }
    return writer.length();
}

}  // namespace

int main() {
    uint8_t request[1024] = {};
    const size_t request_length = make_job(request, sizeof(request), nullptr);
    CHECK_TRUE(request_length != 0);

    BackendState backend_state;
    IppPrintBackend backend{&backend_state, backend_begin, backend_write,
                            backend_end, backend_cancel};
    IppJobStore jobs;
    IppPrintJobLimits limits;
    limits.printer_uri = "ipp://printer.local/ipp/print";
    limits.document_format = "image/urf";
    limits.max_document_bytes = 64;
    IppPrintJobStream stream(&jobs, backend, limits);

    const char body[] = "hello-airprint";
    uint8_t request_with_body[1024] = {};
    const size_t request_with_body_length =
        make_job(request_with_body, sizeof(request_with_body), body);
    CHECK_TRUE(request_with_body_length > request_length);

    IppPrintFeedResult feed = stream.feed(request_with_body, request_with_body_length, 100);
    CHECK_TRUE(feed.error == IppPrintStreamError::Ok);
    CHECK_TRUE(feed.metadata_complete);
    CHECK_TRUE(feed.consumed < request_with_body_length);
    IppPrintFeedResult body_feed = stream.feed(
        request_with_body + feed.consumed, request_with_body_length - feed.consumed, 101);
    CHECK_TRUE(body_feed.error == IppPrintStreamError::Ok);
    CHECK_TRUE(body_feed.consumed == request_with_body_length - feed.consumed);
    CHECK_EQ(stream.document_bytes(), std::strlen(body));
    CHECK_TRUE(backend_state.begin_called);
    CHECK_EQ_MEM(backend_state.bytes, body, std::strlen(body));
    CHECK_TRUE(stream.state() == IppPrintStreamState::ReceivingDocument);
    CHECK_TRUE(stream.finish(200) == IppPrintStreamError::Ok);
    CHECK_TRUE(stream.state() == IppPrintStreamState::Completed);
    CHECK_TRUE(backend_state.end_success);

    IppJobSnapshot completed{};
    CHECK_TRUE(jobs.get_last(&completed) == IppJobError::Ok);
    CHECK_TRUE(completed.state == IppJobState::Completed);
    CHECK_EQ(completed.pages_completed, 1u);

    uint8_t uppercase_request[1024] = {};
    const size_t uppercase_length = make_job(uppercase_request, sizeof(uppercase_request),
                                             body, "IMAGE/URF");
    BackendState uppercase_backend;
    IppPrintBackend uppercase_sink{&uppercase_backend, backend_begin, backend_write,
                                   backend_end, backend_cancel};
    IppJobStore uppercase_jobs;
    IppPrintJobStream uppercase(&uppercase_jobs, uppercase_sink, limits);
    IppPrintFeedResult uppercase_feed =
        uppercase.feed(uppercase_request, uppercase_length, 250);
    CHECK_TRUE(uppercase_feed.error == IppPrintStreamError::Ok);
    IppPrintFeedResult uppercase_body = uppercase.feed(
        uppercase_request + uppercase_feed.consumed,
        uppercase_length - uppercase_feed.consumed, 251);
    CHECK_TRUE(uppercase_body.error == IppPrintStreamError::Ok);
    CHECK_TRUE(uppercase.finish(252) == IppPrintStreamError::Ok);

    BackendState second_backend;
    IppPrintBackend second_sink{&second_backend, backend_begin, backend_write,
                                backend_end, backend_cancel};
    IppPrintJobStream second(&jobs, second_sink, limits);
    feed = second.feed(request_with_body, request_with_body_length, 300);
    CHECK_TRUE(feed.error == IppPrintStreamError::Ok);
    body_feed = second.feed(request_with_body + feed.consumed,
                            request_with_body_length - feed.consumed, 301);
    CHECK_TRUE(body_feed.error == IppPrintStreamError::Ok);
    CHECK_TRUE(second.cancel(310) == IppPrintStreamError::Ok);
    CHECK_TRUE(second.cancel(311) == IppPrintStreamError::Ok);
    CHECK_TRUE(second_backend.cancel_called);
    CHECK_TRUE(second.state() == IppPrintStreamState::Canceled);

    IppPrintJobStream empty(&jobs, backend, limits);
    const size_t metadata_only = make_job(request, sizeof(request), nullptr);
    CHECK_TRUE(empty.feed(request, metadata_only, 400).error == IppPrintStreamError::Ok);
    CHECK_TRUE(empty.finish(401) == IppPrintStreamError::EmptyDocument);

    IppJobStore failed_jobs;
    BackendState failed_backend;
    failed_backend.fail_write = true;
    IppPrintBackend failed_sink{&failed_backend, backend_begin, backend_write,
                                backend_end, backend_cancel};
    IppPrintJobStream failed(&failed_jobs, failed_sink, limits);
    feed = failed.feed(request_with_body, request_with_body_length, 500);
    body_feed = failed.feed(request_with_body + feed.consumed,
                            request_with_body_length - feed.consumed, 501);
    CHECK_TRUE(body_feed.error == IppPrintStreamError::BackendError);
    CHECK_TRUE(failed_backend.cancel_called);

    IppJobStore cancel_failed_jobs;
    BackendState cancel_failed_backend;
    IppPrintBackend cancel_failed_sink{&cancel_failed_backend, backend_begin, backend_write,
                                       backend_end, backend_cancel};
    IppPrintJobStream cancel_failed(&cancel_failed_jobs, cancel_failed_sink, limits);
    feed = cancel_failed.feed(request_with_body, request_with_body_length, 550);
    body_feed = cancel_failed.feed(request_with_body + feed.consumed,
                                   request_with_body_length - feed.consumed, 551);
    CHECK_TRUE(body_feed.error == IppPrintStreamError::Ok);
    cancel_failed_backend.fail_cancel = true;
    CHECK_TRUE(cancel_failed.cancel(552) == IppPrintStreamError::BackendError);
    CHECK_TRUE(cancel_failed.state() == IppPrintStreamState::Failed);
    CHECK_TRUE(cancel_failed.job().state == IppJobState::Aborted);

    IppPrintJobLimits tiny_limits = limits;
    tiny_limits.max_document_bytes = 2;
    IppJobStore tiny_jobs;
    IppPrintJobStream tiny(&tiny_jobs, backend, tiny_limits);
    feed = tiny.feed(request_with_body, request_with_body_length, 600);
    body_feed = tiny.feed(request_with_body + feed.consumed,
                          request_with_body_length - feed.consumed, 601);
    CHECK_TRUE(body_feed.error == IppPrintStreamError::BodyTooLarge);

    return ::ijpd::test::summary("test_ipp_print_job");
}
