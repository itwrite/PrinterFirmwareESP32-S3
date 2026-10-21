// ============================================================================
// job_state_machine.h —— 打印任务状态机（Phase 4，规范见任务书第八章）
//
// 状态：IDLE / PREPARING / WAITING_DATA_CONN / RECEIVING / DRAINING /
//       FINALIZING / CANCELLING / COMPLETED / DONE_WITH_WARNING /
//       CANCELLED / ERROR
//
// 合法转换（其余全部拒绝，返回 INVALID_ARGUMENT / NO_ACTIVE_JOB）：
//   IDLE             + StartAccepted      -> PREPARING
//   PREPARING        + PrepareOk          -> WAITING_DATA_CONN
//   PREPARING        + PrepareFailed      -> ERROR
//   PREPARING        + CancelRequested    -> CANCELLING
//   PREPARING        + FatalError         -> ERROR
//   WAITING_DATA_CONN+ HandshakeOk        -> RECEIVING
//   WAITING_DATA_CONN+ DataConnTimeout    -> ERROR
//   WAITING_DATA_CONN+ CancelRequested    -> CANCELLING
//   WAITING_DATA_CONN+ FatalError         -> ERROR
//   RECEIVING        + AllPacketsReceived -> DRAINING
//   RECEIVING        + CancelRequested    -> CANCELLING
//   RECEIVING        + FatalError         -> ERROR
//   DRAINING         + AllPrinted         -> FINALIZING
//   DRAINING         + CancelRequested    -> CANCELLING
//   DRAINING         + FatalError         -> ERROR
//   FINALIZING       + FinishOk           -> COMPLETED
//   FINALIZING       + FinishWarning      -> DONE_WITH_WARNING
//   FINALIZING       + CancelRequested    -> CANCELLING
//   FINALIZING       + FatalError         -> ERROR
//   CANCELLING       + CancelDone         -> CANCELLED
//   CANCELLING       + FatalError         -> ERROR
//   CANCELLING       + CancelRequested    -> （幂等，状态不变，返回 OK）
//   终态 ×4           + StartAccepted      -> PREPARING（下次 start_print）
//   IDLE/终态        + CancelRequested    -> NO_ACTIVE_JOB
//
// 终态（COMPLETED/DONE_WITH_WARNING/CANCELLED/ERROR）保持到下一次
// StartAccepted，期间可被 get_print_status 查询。
//
// 线程安全性：fire() 多线程安全（CAS）；任意时刻仅一次成功状态变更。
//   last_error() 为日志级近似（宽松读写）。
// 最大耗时：O(1) 无锁。
// ============================================================================
#ifndef IJPD_CORE_JOB_STATE_MACHINE_H
#define IJPD_CORE_JOB_STATE_MACHINE_H

#include <atomic>
#include <cstdint>

#include "core/errors.h"

namespace ijpd {

enum class JobState : uint8_t {
    Idle = 0,
    Preparing,
    WaitingDataConn,
    Receiving,
    Draining,
    Finalizing,
    Cancelling,
    Completed,
    DoneWithWarning,
    Cancelled,
    Error,
};

const char* job_state_name(JobState s) noexcept;
bool job_state_is_terminal(JobState s) noexcept;
bool job_state_is_active(JobState s) noexcept;  // 非终态且非 Idle

enum class JobEvent : uint8_t {
    StartAccepted = 0,
    PrepareOk,
    PrepareFailed,
    HandshakeOk,
    DataConnTimeout,
    AllPacketsReceived,
    AllPrinted,
    FinishOk,
    FinishWarning,
    CancelRequested,
    CancelDone,
    FatalError,
};

const char* job_event_name(JobEvent e) noexcept;

class JobStateMachine {
public:
    JobStateMachine() noexcept = default;

    // 触发转换。code：进入 ERROR/DONE_WITH_WARNING 时携带的错误/警告码。
    // 返回 OK（含幂等取消）或拒绝原因（INVALID_ARGUMENT/NO_ACTIVE_JOB）。
    Err fire(JobEvent e, Err code = Err::OK) noexcept;

    JobState state() const noexcept { return state_.load(std::memory_order_acquire); }
    Err last_error() const noexcept { return last_error_.load(std::memory_order_relaxed); }

    // 成功的状态“变更”次数（幂等取消不计；观测/测试用）
    uint32_t transitions() const noexcept {
        return transitions_.load(std::memory_order_relaxed);
    }

private:
    // 决策表：合法 -> true 并给出下一状态；幂等取消 -> *idempotent = true
    static bool decide(JobState cur, JobEvent e, JobState* next,
                       Err* reject, bool* idempotent) noexcept;

    std::atomic<JobState> state_{JobState::Idle};
    std::atomic<Err> last_error_{Err::OK};
    std::atomic<uint32_t> transitions_{0};
};

}  // namespace ijpd

#endif  // IJPD_CORE_JOB_STATE_MACHINE_H
