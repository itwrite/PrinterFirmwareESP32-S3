// job_state_machine.cpp —— 单点状态转换（CAS 保证任意时刻仅一次生效）
#include "core/job_state_machine.h"

namespace ijpd {

const char* job_state_name(JobState s) noexcept {
    switch (s) {
        case JobState::Idle:            return "idle";
        case JobState::Preparing:       return "preparing";
        case JobState::WaitingDataConn: return "waiting_data_conn";
        case JobState::Receiving:       return "receiving";
        case JobState::Draining:        return "draining";
        case JobState::Finalizing:      return "finalizing";
        case JobState::Cancelling:      return "cancelling";
        case JobState::Completed:       return "completed";
        case JobState::DoneWithWarning: return "done_with_warning";
        case JobState::Cancelled:       return "cancelled";
        case JobState::Error:           return "error";
        default:                        return "unknown";
    }
}

bool job_state_is_terminal(JobState s) noexcept {
    return s == JobState::Completed || s == JobState::DoneWithWarning ||
           s == JobState::Cancelled || s == JobState::Error;
}

bool job_state_is_active(JobState s) noexcept {
    return s != JobState::Idle && !job_state_is_terminal(s);
}

const char* job_event_name(JobEvent e) noexcept {
    switch (e) {
        case JobEvent::StartAccepted:      return "start_accepted";
        case JobEvent::PrepareOk:          return "prepare_ok";
        case JobEvent::PrepareFailed:      return "prepare_failed";
        case JobEvent::HandshakeOk:        return "handshake_ok";
        case JobEvent::DataConnTimeout:    return "data_conn_timeout";
        case JobEvent::AllPacketsReceived: return "all_packets_received";
        case JobEvent::AllPrinted:         return "all_printed";
        case JobEvent::FinishOk:           return "finish_ok";
        case JobEvent::FinishWarning:      return "finish_warning";
        case JobEvent::CancelRequested:    return "cancel_requested";
        case JobEvent::CancelDone:         return "cancel_done";
        case JobEvent::FatalError:         return "fatal_error";
        default:                           return "unknown";
    }
}

bool JobStateMachine::decide(JobState cur, JobEvent e, JobState* next,
                             Err* reject, bool* idempotent) noexcept {
    *idempotent = false;
    *next = cur;
    *reject = Err::OK;

    // 取消：无任务（IDLE/终态） -> NO_ACTIVE_JOB
    if (e == JobEvent::CancelRequested) {
        if (cur == JobState::Cancelling) {
            *idempotent = true;  // 幂等：重复取消
            return true;
        }
        if (job_state_is_active(cur)) {
            *next = JobState::Cancelling;
            return true;
        }
        *reject = Err::NO_ACTIVE_JOB;
        return false;
    }

    // 新任务：IDLE 或任意终态均可启动
    if (e == JobEvent::StartAccepted) {
        if (cur == JobState::Idle || job_state_is_terminal(cur)) {
            *next = JobState::Preparing;
            return true;
        }
        *reject = Err::JOB_ALREADY_RUNNING;
        return false;
    }

    switch (cur) {
        case JobState::Preparing:
            if (e == JobEvent::PrepareOk) { *next = JobState::WaitingDataConn; return true; }
            if (e == JobEvent::PrepareFailed) { *next = JobState::Error; return true; }
            if (e == JobEvent::FatalError) { *next = JobState::Error; return true; }
            break;
        case JobState::WaitingDataConn:
            if (e == JobEvent::HandshakeOk) { *next = JobState::Receiving; return true; }
            if (e == JobEvent::DataConnTimeout) { *next = JobState::Error; return true; }
            if (e == JobEvent::FatalError) { *next = JobState::Error; return true; }
            break;
        case JobState::Receiving:
            if (e == JobEvent::AllPacketsReceived) { *next = JobState::Draining; return true; }
            if (e == JobEvent::FatalError) { *next = JobState::Error; return true; }
            break;
        case JobState::Draining:
            if (e == JobEvent::AllPrinted) { *next = JobState::Finalizing; return true; }
            if (e == JobEvent::FatalError) { *next = JobState::Error; return true; }
            break;
        case JobState::Finalizing:
            if (e == JobEvent::FinishOk) { *next = JobState::Completed; return true; }
            if (e == JobEvent::FinishWarning) { *next = JobState::DoneWithWarning; return true; }
            if (e == JobEvent::FatalError) { *next = JobState::Error; return true; }
            break;
        case JobState::Cancelling:
            if (e == JobEvent::CancelDone) { *next = JobState::Cancelled; return true; }
            if (e == JobEvent::FatalError) { *next = JobState::Error; return true; }
            break;
        default:
            break;
    }
    *reject = Err::INVALID_ARGUMENT;
    return false;
}

Err JobStateMachine::fire(JobEvent e, Err code) noexcept {
    JobState cur = state_.load(std::memory_order_acquire);
    for (;;) {
        JobState next;
        Err reject;
        bool idempotent;
        if (!decide(cur, e, &next, &reject, &idempotent)) {
            return reject;
        }
        if (idempotent) return Err::OK;

        if (next == cur) return Err::OK;  // 不变（防御）

        JobState expected = cur;
        if (state_.compare_exchange_strong(expected, next, std::memory_order_acq_rel)) {
            if (next == JobState::Error || next == JobState::DoneWithWarning ||
                next == JobState::Cancelled) {
                last_error_.store(code != Err::OK ? code : Err::INTERNAL_ERROR,
                                  std::memory_order_relaxed);
            }
            transitions_.fetch_add(1, std::memory_order_relaxed);
            return Err::OK;
        }
        cur = expected;  // CAS 失败：用返回的最新值重试
    }
}

}  // namespace ijpd
