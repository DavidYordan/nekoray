#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace NekoGui_Runtime {
    enum class TransitionKind {
        Start,
        Stop,
        CrashCleanup,
    };

    struct TransitionTicket {
        std::uint64_t generation = 0;
        TransitionKind kind = TransitionKind::Start;
        bool valid = false;
    };

    struct TransitionCompletion {
        bool completed = false;
        TransitionTicket handoff;
    };

    // Process-local fencing for GUI/core transitions. The coordinator owns no
    // OS state: it only guarantees that one Start/Stop command is in flight
    // and that a stale worker completion cannot release a newer transition.
    class TransitionCoordinator {
    public:
        // When depthGate is supplied, its value is updated while holding the
        // same mutex that decides ownership: 1 means a transition is owned or
        // crash cleanup is pending, and 0 means the coordinator is quiescent.
        // In particular, a rejected acquisition must not clear the gate while
        // another generation (or pending cleanup) still owns the fence.
        [[nodiscard]] TransitionTicket TryBegin(
            TransitionKind kind,
            std::atomic_int* depthGate = nullptr);

        [[nodiscard]] bool IsCurrent(const TransitionTicket& ticket) const;

        // Record a pending crash cleanup before trying to acquire it. Normal
        // Start/Stop transitions remain fenced while this flag is set.
        void RequestCrashCleanup(std::atomic_int* depthGate = nullptr);

        // Completes ticket and, when a crash cleanup was requested, transfers
        // ownership directly to a new CrashCleanup generation without ever
        // exposing an idle coordinator in between.
        [[nodiscard]] TransitionCompletion CompleteOrHandoff(
            const TransitionTicket& ticket,
            std::atomic_int* depthGate = nullptr);

        [[nodiscard]] bool Busy() const;

        [[nodiscard]] std::uint64_t Generation() const;

    private:
        mutable std::mutex mutex;
        std::uint64_t nextGeneration = 0;
        std::uint64_t currentGeneration = 0;
        TransitionKind currentKind = TransitionKind::Start;
        bool busy = false;
        bool crashCleanupPending = false;

        void UpdateDepthGateLocked(std::atomic_int* depthGate) const;
    };

    struct DaemonProfileStartRequest {
        std::uint64_t daemonGeneration = 0;
        std::uint64_t requestGeneration = 0;
        int profileId = -1;
        bool valid = false;
    };

    struct DaemonInstanceSnapshot {
        std::uint64_t generation = 0;
        std::string instanceId;
        bool ready = false;

        [[nodiscard]] bool valid() const {
            return generation != 0 && !instanceId.empty();
        }
    };

    struct DaemonReadyResult {
        bool accepted = false;
        DaemonProfileStartRequest profileStart;
    };

    // Pure generation bookkeeping for QProcess restart timers and a profile
    // queued for the next daemon. CoreProcess owns the actual process; this
    // state prevents a timer belonging to an old daemon from killing a newer
    // one and lets an explicit Stop cancel a queued profile start.
    class DaemonGenerationState {
    public:
        [[nodiscard]] std::uint64_t BeginProcessStart(const std::string& instanceId);

        // Atomically queues a profile for a starting daemon, or returns an
        // immediately consumable request when that daemon is already ready.
        // This closes the IsReady(false) -> queue race in which readiness used
        // to arrive before the queued profile could be emitted.
        [[nodiscard]] DaemonProfileStartRequest QueueProfileForNextStart(int profileId);

        [[nodiscard]] bool CancelQueuedProfile();

        // Marks the current daemon ready and moves a queued request into an
        // emitted-but-not-consumed state. Explicit Stop can still invalidate
        // that state before the UI consumes its event.
        [[nodiscard]] DaemonReadyResult MarkProcessReady(
            std::uint64_t expectedGeneration,
            const std::string& expectedInstanceId);

        [[nodiscard]] std::uint64_t MarkProcessStopped();

        [[nodiscard]] bool ConsumeReadyProfile(
            const DaemonProfileStartRequest& request);

        [[nodiscard]] bool IsReady(std::uint64_t expectedGeneration) const;

        [[nodiscard]] std::uint64_t CrashRestartToken() const;

        [[nodiscard]] bool CanRunCrashRestart(std::uint64_t token) const;

        [[nodiscard]] std::uint64_t CurrentGeneration() const;

        [[nodiscard]] DaemonInstanceSnapshot CurrentInstance() const;

    private:
        mutable std::mutex mutex;
        std::uint64_t generation = 0;
        std::string instanceId;
        std::uint64_t nextProfileRequestGeneration = 0;
        bool ready = false;
        DaemonProfileStartRequest queuedProfile;
        DaemonProfileStartRequest emittedProfile;
    };
} // namespace NekoGui_Runtime
