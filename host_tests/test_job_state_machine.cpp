// test_job_state_machine.cpp —— 表驱动全覆盖合法/非法转换 + 并发竞争
#include "core/job_state_machine.h"
#include "assert.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace ijpd;

namespace {

struct LegalRow {
    JobState from;
    JobEvent ev;
    JobState to;
};

// 全部合法转换（与 job_state_machine.h 文档表一致）
const LegalRow kLegal[] = {
    {JobState::Idle, JobEvent::StartAccepted, JobState::Preparing},
    {JobState::Preparing, JobEvent::PrepareOk, JobState::WaitingDataConn},
    {JobState::Preparing, JobEvent::PrepareFailed, JobState::Error},
    {JobState::Preparing, JobEvent::CancelRequested, JobState::Cancelling},
    {JobState::Preparing, JobEvent::FatalError, JobState::Error},
    {JobState::WaitingDataConn, JobEvent::HandshakeOk, JobState::Receiving},
    {JobState::WaitingDataConn, JobEvent::DataConnTimeout, JobState::Error},
    {JobState::WaitingDataConn, JobEvent::CancelRequested, JobState::Cancelling},
    {JobState::WaitingDataConn, JobEvent::FatalError, JobState::Error},
    {JobState::Receiving, JobEvent::AllPacketsReceived, JobState::Draining},
    {JobState::Receiving, JobEvent::CancelRequested, JobState::Cancelling},
    {JobState::Receiving, JobEvent::FatalError, JobState::Error},
    {JobState::Draining, JobEvent::AllPrinted, JobState::Finalizing},
    {JobState::Draining, JobEvent::CancelRequested, JobState::Cancelling},
    {JobState::Draining, JobEvent::FatalError, JobState::Error},
    {JobState::Finalizing, JobEvent::FinishOk, JobState::Completed},
    {JobState::Finalizing, JobEvent::FinishWarning, JobState::DoneWithWarning},
    {JobState::Finalizing, JobEvent::CancelRequested, JobState::Cancelling},
    {JobState::Finalizing, JobEvent::FatalError, JobState::Error},
    {JobState::Cancelling, JobEvent::CancelDone, JobState::Cancelled},
    {JobState::Cancelling, JobEvent::FatalError, JobState::Error},
    {JobState::Completed, JobEvent::StartAccepted, JobState::Preparing},
    {JobState::DoneWithWarning, JobEvent::StartAccepted, JobState::Preparing},
    {JobState::Cancelled, JobEvent::StartAccepted, JobState::Preparing},
    {JobState::Error, JobEvent::StartAccepted, JobState::Preparing},
};

bool find_legal(JobState s, JobEvent e, JobState* to) {
    for (const auto& r : kLegal) {
        if (r.from == s && r.ev == e) {
            *to = r.to;
            return true;
        }
    }
    return false;
}

const JobEvent kEvents[] = {JobEvent::StartAccepted,   JobEvent::PrepareOk,
                            JobEvent::PrepareFailed,   JobEvent::HandshakeOk,
                            JobEvent::DataConnTimeout, JobEvent::AllPacketsReceived,
                            JobEvent::AllPrinted,      JobEvent::FinishOk,
                            JobEvent::FinishWarning,   JobEvent::CancelRequested,
                            JobEvent::CancelDone,      JobEvent::FatalError};

}  // namespace

int main() {
    // ================= 1. 表驱动：11 状态 × 12 事件 全覆盖 =================
    {
        // 从 Idle 抵达各状态的合法路径
        auto reach = [](JobStateMachine& m, JobState target) -> bool {
            auto f = [&](JobEvent e) { return m.fire(e) == Err::OK; };
            switch (target) {
                case JobState::Idle:            return true;
                case JobState::Preparing:       return f(JobEvent::StartAccepted);
                case JobState::WaitingDataConn: return f(JobEvent::StartAccepted) && f(JobEvent::PrepareOk);
                case JobState::Receiving:
                    return f(JobEvent::StartAccepted) && f(JobEvent::PrepareOk) &&
                           f(JobEvent::HandshakeOk);
                case JobState::Draining:
                    return f(JobEvent::StartAccepted) && f(JobEvent::PrepareOk) &&
                           f(JobEvent::HandshakeOk) && f(JobEvent::AllPacketsReceived);
                case JobState::Finalizing:
                    return f(JobEvent::StartAccepted) && f(JobEvent::PrepareOk) &&
                           f(JobEvent::HandshakeOk) && f(JobEvent::AllPacketsReceived) &&
                           f(JobEvent::AllPrinted);
                case JobState::Cancelling:
                    return f(JobEvent::StartAccepted) && f(JobEvent::PrepareOk) &&
                           f(JobEvent::CancelRequested);
                case JobState::Completed:
                    return f(JobEvent::StartAccepted) && f(JobEvent::PrepareOk) &&
                           f(JobEvent::HandshakeOk) && f(JobEvent::AllPacketsReceived) &&
                           f(JobEvent::AllPrinted) && f(JobEvent::FinishOk);
                case JobState::DoneWithWarning:
                    return f(JobEvent::StartAccepted) && f(JobEvent::PrepareOk) &&
                           f(JobEvent::HandshakeOk) && f(JobEvent::AllPacketsReceived) &&
                           f(JobEvent::AllPrinted) && f(JobEvent::FinishWarning);
                case JobState::Cancelled:
                    return f(JobEvent::StartAccepted) && f(JobEvent::PrepareOk) &&
                           f(JobEvent::CancelRequested) && f(JobEvent::CancelDone);
                case JobState::Error:
                    return f(JobEvent::StartAccepted) && f(JobEvent::PrepareFailed);
                default:                  return false;
            }
        };

        int legal_checked = 0;
        int rejected_checked = 0;
        for (int si = 0; si < 11; ++si) {
            for (size_t ei = 0; ei < sizeof(kEvents) / sizeof(kEvents[0]); ++ei) {
                const JobState s = static_cast<JobState>(si);
                const JobEvent e = kEvents[ei];
                JobStateMachine m;
                CHECK_TRUE(reach(m, s));
                CHECK_TRUE(m.state() == s);  // 抵达确认

                JobState to;
                if (find_legal(s, e, &to)) {
                    // 合法转换：必须成功且抵达目标
                    CHECK_TRUE(m.fire(e) == Err::OK);
                    CHECK_TRUE(m.state() == to);
                    ++legal_checked;
                } else if (s == JobState::Cancelling && e == JobEvent::CancelRequested) {
                    // 幂等取消：成功但状态不变
                    CHECK_TRUE(m.fire(e) == Err::OK);
                    CHECK_TRUE(m.state() == JobState::Cancelling);
                    ++legal_checked;
                } else {
                    // 非法转换：必须拒绝（任意非 OK 错误码）
                    CHECK_TRUE(m.fire(e) != Err::OK);
                    CHECK_TRUE(m.state() == s);  // 状态不变
                    ++rejected_checked;
                }
            }
        }
        CHECK_EQ(legal_checked, static_cast<int>(sizeof(kLegal) / sizeof(kLegal[0])) + 1);
        CHECK_EQ(rejected_checked, 11 * 12 - legal_checked);
    }

    // ================= 2. 每条合法路径实跑验证 =================
    {
        struct Path { std::vector<JobEvent> evs; JobState want; Err code; };
        const std::vector<Path> paths = {
            {{JobEvent::StartAccepted, JobEvent::PrepareOk, JobEvent::HandshakeOk,
              JobEvent::AllPacketsReceived, JobEvent::AllPrinted, JobEvent::FinishOk},
             JobState::Completed, Err::OK},
            {{JobEvent::StartAccepted, JobEvent::PrepareOk, JobEvent::HandshakeOk,
              JobEvent::AllPacketsReceived, JobEvent::AllPrinted, JobEvent::FinishWarning},
             JobState::DoneWithWarning, Err::UNSUPPORTED},
            {{JobEvent::StartAccepted, JobEvent::PrepareOk, JobEvent::HandshakeOk,
              JobEvent::AllPacketsReceived, JobEvent::AllPrinted,
              JobEvent::CancelRequested, JobEvent::CancelDone},
             JobState::Cancelled, Err::JOB_CANCELLED},
            {{JobEvent::StartAccepted, JobEvent::PrepareFailed},
             JobState::Error, Err::PSRAM_UNAVAILABLE},
            {{JobEvent::StartAccepted, JobEvent::PrepareOk, JobEvent::DataConnTimeout},
             JobState::Error, Err::JOB_TIMEOUT},
            {{JobEvent::StartAccepted, JobEvent::PrepareOk, JobEvent::HandshakeOk,
              JobEvent::FatalError},
             JobState::Error, Err::DATA_CONNECTION_LOST},
            {{JobEvent::StartAccepted, JobEvent::PrepareOk, JobEvent::HandshakeOk,
              JobEvent::AllPacketsReceived, JobEvent::CancelRequested,
              JobEvent::CancelDone},
             JobState::Cancelled, Err::JOB_CANCELLED},
            {{JobEvent::StartAccepted, JobEvent::CancelRequested, JobEvent::CancelDone},
             JobState::Cancelled, Err::JOB_CANCELLED},
        };
        for (const auto& p : paths) {
            JobStateMachine m;
            for (size_t i = 0; i < p.evs.size(); ++i) {
                const Err code = (i == p.evs.size() - 1) ? p.code : Err::OK;
                CHECK_TRUE(m.fire(p.evs[i], code) == Err::OK);
            }
            CHECK_TRUE(m.state() == p.want);
            if (p.want == JobState::Error || p.want == JobState::DoneWithWarning ||
                p.want == JobState::Cancelled) {
                CHECK_TRUE(m.last_error() == p.code);
            }
            // 终态保持：可查询、拒绝无关事件
            CHECK_TRUE(job_state_is_terminal(m.state()));
            CHECK_TRUE(m.fire(JobEvent::HandshakeOk) == Err::INVALID_ARGUMENT);
            CHECK_TRUE(m.fire(JobEvent::AllPrinted) == Err::INVALID_ARGUMENT);
            // 终态可再次启动
            CHECK_TRUE(m.fire(JobEvent::StartAccepted) == Err::OK);
            CHECK_TRUE(m.state() == JobState::Preparing);
        }
    }

    // ================= 3. 非法转换与取消特例 =================
    {
        JobStateMachine m;
        CHECK_TRUE(m.fire(JobEvent::CancelRequested) == Err::NO_ACTIVE_JOB);  // IDLE
        CHECK_TRUE(m.fire(JobEvent::PrepareOk) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(m.fire(JobEvent::HandshakeOk) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(m.fire(JobEvent::StartAccepted) == Err::OK);
        CHECK_TRUE(m.fire(JobEvent::StartAccepted) == Err::JOB_ALREADY_RUNNING);  // 重复启动
        CHECK_TRUE(m.fire(JobEvent::HandshakeOk) == Err::INVALID_ARGUMENT);  // PREPARING
        CHECK_TRUE(m.fire(JobEvent::PrepareOk) == Err::OK);
        CHECK_TRUE(m.fire(JobEvent::AllPacketsReceived) == Err::INVALID_ARGUMENT);  // 未接收完
        CHECK_TRUE(m.fire(JobEvent::HandshakeOk) == Err::OK);
        // CANCELLING 幂等取消
        CHECK_TRUE(m.fire(JobEvent::CancelRequested) == Err::OK);
        CHECK_TRUE(m.state() == JobState::Cancelling);
        CHECK_TRUE(m.fire(JobEvent::CancelRequested) == Err::OK);  // 幂等
        CHECK_TRUE(m.state() == JobState::Cancelling);
        CHECK_TRUE(m.fire(JobEvent::AllPacketsReceived) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(m.fire(JobEvent::CancelDone) == Err::OK);
        CHECK_TRUE(m.state() == JobState::Cancelled);
        // 终态取消 -> NO_ACTIVE_JOB
        CHECK_TRUE(m.fire(JobEvent::CancelRequested) == Err::NO_ACTIVE_JOB);
    }

    // ================= 4. 并发竞争：取消仅一次生效，无非法状态 =================
    {
        for (int round = 0; round < 200; ++round) {
            JobStateMachine m;
            CHECK_TRUE(m.fire(JobEvent::StartAccepted) == Err::OK);
            CHECK_TRUE(m.fire(JobEvent::PrepareOk) == Err::OK);
            std::vector<std::thread> ts;
            for (int t = 0; t < 4; ++t) {
                ts.emplace_back([&]() {
                    for (int i = 0; i < 50; ++i) {
                        const Err r = m.fire(JobEvent::CancelRequested);
                        CHECK_TRUE(r == Err::OK);  // 首次或幂等
                        std::this_thread::yield();
                    }
                });
            }
            // 同时有竞争者尝试正常推进
            ts.emplace_back([&]() {
                for (int i = 0; i < 50; ++i) {
                    const Err r = m.fire(JobEvent::HandshakeOk);
                    CHECK_TRUE(r == Err::OK || r == Err::INVALID_ARGUMENT);
                    std::this_thread::yield();
                }
            });
            for (auto& t : ts) t.join();
            // 结果必须是 Cancelling 或 Receiving（HandshakeOk 抢先）
            const JobState s = m.state();
            CHECK_TRUE(s == JobState::Cancelling || s == JobState::Receiving);
            // 结果必须是 Cancelling 或 Receiving（HandshakeOk 抢先）。
            // 合法竞态：HandshakeOk 先生效后取消仍可从 RECEIVING 进入 CANCELLING，
            // 故转换数为 3（未握手即取消）或 4（握手后取消）。
            if (s == JobState::Receiving) {
                CHECK_EQ(m.transitions(), 3u);
            } else {
                CHECK_TRUE(m.transitions() == 3u || m.transitions() == 4u);
            }
        }
    }

    // ================= 5. 名称函数 =================
    {
        CHECK_EQ_STR(job_state_name(JobState::WaitingDataConn), "waiting_data_conn");
        CHECK_EQ_STR(job_state_name(JobState::DoneWithWarning), "done_with_warning");
        CHECK_EQ_STR(job_event_name(JobEvent::AllPacketsReceived), "all_packets_received");
        CHECK_TRUE(job_state_is_terminal(JobState::Completed));
        CHECK_TRUE(!job_state_is_terminal(JobState::Cancelling));
        CHECK_TRUE(job_state_is_active(JobState::Receiving));
        CHECK_TRUE(!job_state_is_active(JobState::Idle));
        CHECK_TRUE(!job_state_is_active(JobState::Error));
    }

    return ::ijpd::test::summary("test_job_state_machine");
}
