#include "RuntimeTransition.hpp"

namespace NekoGui_Runtime {
    namespace {
        bool sameProcess(
            const DaemonProcessSnapshot& left,
            const DaemonProcessSnapshot& right) {
            return left.valid() && right.valid() &&
                   left.generation == right.generation &&
                   left.instanceId == right.instanceId &&
                   left.processId == right.processId;
        }
    }

    void TransitionCoordinator::UpdateDepthGateLocked(std::atomic_int* depthGate) const {
        if (depthGate != nullptr) depthGate->store(busy || crashCleanupPending ? 1 : 0);
    }

    TransitionTicket TransitionCoordinator::TryBegin(
        TransitionKind kind,
        std::atomic_int* depthGate) {
        std::lock_guard<std::mutex> lock(mutex);
        if (busy || (crashCleanupPending && kind != TransitionKind::CrashCleanup)) {
            UpdateDepthGateLocked(depthGate);
            return {};
        }
        if (kind == TransitionKind::CrashCleanup) {
            if (!crashCleanupPending) {
                UpdateDepthGateLocked(depthGate);
                return {};
            }
            crashCleanupPending = false;
        }

        busy = true;
        currentKind = kind;
        currentGeneration = ++nextGeneration;
        UpdateDepthGateLocked(depthGate);
        return {currentGeneration, currentKind, true};
    }

    bool TransitionCoordinator::IsCurrent(const TransitionTicket& ticket) const {
        std::lock_guard<std::mutex> lock(mutex);
        return ticket.valid && busy && ticket.generation == currentGeneration &&
               ticket.kind == currentKind;
    }

    void TransitionCoordinator::RequestCrashCleanup(std::atomic_int* depthGate) {
        std::lock_guard<std::mutex> lock(mutex);
        crashCleanupPending = true;
        UpdateDepthGateLocked(depthGate);
    }

    TransitionCompletion TransitionCoordinator::CompleteOrHandoff(
        const TransitionTicket& ticket,
        std::atomic_int* depthGate) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!ticket.valid || !busy || ticket.generation != currentGeneration ||
            ticket.kind != currentKind) {
            UpdateDepthGateLocked(depthGate);
            return {};
        }

        TransitionCompletion result;
        result.completed = true;
        if (crashCleanupPending) {
            crashCleanupPending = false;
            currentKind = TransitionKind::CrashCleanup;
            currentGeneration = ++nextGeneration;
            result.handoff = {currentGeneration, currentKind, true};
            UpdateDepthGateLocked(depthGate);
            return result;
        }

        busy = false;
        UpdateDepthGateLocked(depthGate);
        return result;
    }

    bool TransitionCoordinator::Busy() const {
        std::lock_guard<std::mutex> lock(mutex);
        return busy;
    }

    std::uint64_t TransitionCoordinator::Generation() const {
        std::lock_guard<std::mutex> lock(mutex);
        return currentGeneration;
    }

    std::uint64_t DaemonGenerationState::BeginProcessStart(const std::string& newInstanceId) {
        std::lock_guard<std::mutex> lock(mutex);
        ++generation;
        instanceId = newInstanceId;
        ready = false;
        emittedProfile = {};
        if (queuedProfile.valid) queuedProfile.daemonGeneration = generation;
        return generation;
    }

    DaemonProfileStartRequest DaemonGenerationState::QueueProfileForNextStart(int profileId) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto requestGeneration = ++nextProfileRequestGeneration;
        queuedProfile = {generation, requestGeneration, profileId, profileId >= 0};
        // A newer explicit request invalidates an older ready event that has
        // not yet been consumed by the UI.
        emittedProfile = {};
        if (ready && generation != 0 && queuedProfile.valid) {
            emittedProfile = queuedProfile;
            queuedProfile = {};
            return emittedProfile;
        }
        // If a daemon is already starting, bind to that generation. If it is
        // actually replaced before readiness, BeginProcessStart rebinds the
        // same request to the replacement generation.
        return {};
    }

    bool DaemonGenerationState::CancelQueuedProfile() {
        std::lock_guard<std::mutex> lock(mutex);
        const auto hadQueuedProfile = queuedProfile.valid || emittedProfile.valid;
        queuedProfile = {};
        emittedProfile = {};
        return hadQueuedProfile;
    }

    DaemonReadyResult DaemonGenerationState::MarkProcessReady(
        std::uint64_t expectedGeneration,
        const std::string& expectedInstanceId) {
        std::lock_guard<std::mutex> lock(mutex);
        if (generation == 0 || expectedGeneration != generation ||
            expectedInstanceId.empty() || expectedInstanceId != instanceId) {
            return {};
        }
        if (ready) return {true, {}};
        ready = true;
        if (!queuedProfile.valid || queuedProfile.daemonGeneration != generation) {
            return {true, {}};
        }
        emittedProfile = queuedProfile;
        queuedProfile = {};
        return {true, emittedProfile};
    }

    std::uint64_t DaemonGenerationState::MarkProcessStopped() {
        std::lock_guard<std::mutex> lock(mutex);
        ready = false;
        instanceId.clear();
        queuedProfile = {};
        emittedProfile = {};
        return generation;
    }

    bool DaemonGenerationState::ConsumeReadyProfile(
        const DaemonProfileStartRequest& request) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!request.valid || !ready || request.daemonGeneration != generation ||
            !emittedProfile.valid ||
            request.daemonGeneration != emittedProfile.daemonGeneration ||
            request.requestGeneration != emittedProfile.requestGeneration ||
            request.profileId != emittedProfile.profileId) {
            return false;
        }
        emittedProfile = {};
        return true;
    }

    bool DaemonGenerationState::IsReady(std::uint64_t expectedGeneration) const {
        std::lock_guard<std::mutex> lock(mutex);
        return expectedGeneration != 0 && expectedGeneration == generation && ready;
    }

    std::uint64_t DaemonGenerationState::CrashRestartToken() const {
        std::lock_guard<std::mutex> lock(mutex);
        return generation;
    }

    bool DaemonGenerationState::CanRunCrashRestart(std::uint64_t token) const {
        std::lock_guard<std::mutex> lock(mutex);
        return token != 0 && token == generation;
    }

    std::uint64_t DaemonGenerationState::CurrentGeneration() const {
        std::lock_guard<std::mutex> lock(mutex);
        return generation;
    }

    DaemonInstanceSnapshot DaemonGenerationState::CurrentInstance() const {
        std::lock_guard<std::mutex> lock(mutex);
        return {generation, instanceId, ready};
    }

    bool DaemonProcessExitState::MarkProcessStarted(
        const DaemonProcessSnapshot& process) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!process.valid() || runningProcess.valid()) return false;
        runningProcess = process;
        return true;
    }

    DaemonProcessFinishedResult DaemonProcessExitState::MarkProcessFinished(
        int exitCode,
        bool normalExit) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!runningProcess.valid()) return {};
        lastFinished = {runningProcess, exitCode, normalExit, true};
        runningProcess = {};
        finishedCondition.notify_all();
        return lastFinished;
    }

    DaemonProcessSnapshot DaemonProcessExitState::CurrentProcess() const {
        std::lock_guard<std::mutex> lock(mutex);
        return runningProcess;
    }

    bool DaemonProcessExitState::WaitForFinished(
        const DaemonProcessSnapshot& expected,
        std::chrono::milliseconds timeout,
        DaemonProcessFinishedResult* result) const {
        if (!expected.valid() || timeout.count() < 0) return false;
        std::unique_lock<std::mutex> lock(mutex);
        const auto exactFinished = [&] {
            return lastFinished.valid && sameProcess(lastFinished.process, expected);
        };
        if (!finishedCondition.wait_for(lock, timeout, exactFinished)) return false;
        if (result != nullptr) *result = lastFinished;
        return true;
    }
} // namespace NekoGui_Runtime
