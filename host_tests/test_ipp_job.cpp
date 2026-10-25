// test_ipp_job.cpp - Single-active IPP Job lifecycle tests.
#include "assert.h"

#include <cstring>

#include "airprint_core/ipp_job.h"

using namespace ijpd::airprint;

int main() {
    IppJobStore store;
    IppJobSnapshot snapshot{};
    CHECK_TRUE(!store.active());
    CHECK_TRUE(store.get_active(&snapshot) == IppJobError::NoActiveJob);

    CHECK_TRUE(store.create("ipp://printer.local/ipp/print", "photo", "alice",
                            "image/urf", 100, &snapshot) == IppJobError::Ok);
    CHECK_TRUE(store.active());
    CHECK_TRUE(snapshot.state == IppJobState::Pending);
    CHECK_EQ(snapshot.id, 1u);
    CHECK_EQ_STR(snapshot.printer_uri, "ipp://printer.local/ipp/print");
    CHECK_EQ_STR(snapshot.job_id, "1");
    CHECK_EQ_STR(snapshot.job_uri, "ipp://printer.local/ipp/print/job/1");
    CHECK_EQ_STR(snapshot.job_name, "photo");
    CHECK_EQ_STR(snapshot.user_name, "alice");
    CHECK_EQ_STR(snapshot.document_format, "image/urf");
    CHECK_EQ(snapshot.created_ms, 100u);

    IppJobSnapshot active{};
    CHECK_TRUE(store.get_active(&active) == IppJobError::Ok);
    CHECK_EQ(active.id, snapshot.id);
    CHECK_TRUE(store.create("ipp://printer.local/ipp/print", "second", "bob",
                            "image/urf", 101, &snapshot) == IppJobError::AlreadyActive);
    CHECK_TRUE(store.start_processing(2, 110) == IppJobError::IdMismatch);
    CHECK_TRUE(store.start_processing(1, 110) == IppJobError::Ok);
    CHECK_TRUE(store.complete(1, 120, 1, 7) == IppJobError::Ok);
    CHECK_TRUE(!store.active());
    CHECK_TRUE(store.get_last(&snapshot) == IppJobError::Ok);
    CHECK_TRUE(snapshot.state == IppJobState::Completed);
    CHECK_EQ(snapshot.pages_completed, 1u);
    CHECK_EQ(snapshot.job_k_octets, 7u);
    CHECK_EQ_STR(snapshot.state_reason, "none");

    CHECK_TRUE(store.create("ipp://printer.local/ipp/print", "second", "bob",
                            "image/urf", 200, &snapshot) == IppJobError::Ok);
    CHECK_EQ(snapshot.id, 2u);
    CHECK_TRUE(store.cancel(2, 210) == IppJobError::Ok);
    CHECK_TRUE(store.cancel(2, 211) == IppJobError::Ok);
    CHECK_TRUE(store.get(2, &snapshot) == IppJobError::Ok);
    CHECK_TRUE(snapshot.state == IppJobState::Canceled);
    CHECK_EQ_STR(snapshot.state_reason, "job-canceled-by-user");
    CHECK_TRUE(store.complete(2, 220, 1, 1) == IppJobError::InvalidState);

    CHECK_TRUE(store.create("ipp://printer.local/ipp/print", "third", "carol",
                            "image/urf", 300, &snapshot) == IppJobError::Ok);
    CHECK_TRUE(store.abort(3, "decoder-error", 330) == IppJobError::Ok);
    CHECK_TRUE(store.get_last(&snapshot) == IppJobError::Ok);
    CHECK_TRUE(snapshot.state == IppJobState::Aborted);
    CHECK_EQ_STR(snapshot.state_reason, "decoder-error");

    CHECK_TRUE(store.create("ipp://printer.local/ipp/print", "fourth", "dave",
                            "image/urf", 400, &snapshot) == IppJobError::Ok);
    store.reset();
    CHECK_TRUE(!store.active());
    CHECK_TRUE(store.get_last(&snapshot) == IppJobError::NoActiveJob);

    char too_long[200] = {};
    std::memset(too_long, 'x', sizeof(too_long) - 1);
    CHECK_TRUE(store.create("ipp://printer.local/ipp/print", too_long, "eve",
                            "image/urf", 500, &snapshot) == IppJobError::InvalidArgument);
    return ::ijpd::test::summary("test_ipp_job");
}
