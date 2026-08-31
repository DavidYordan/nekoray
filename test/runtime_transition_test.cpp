#include "main/RuntimeTransition.hpp"

#include <QCoreApplication>
#include <QDebug>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
    bool expect(bool condition, const char* message) {
        if (condition) return true;
        qCritical() << message;
        std::fprintf(stderr, "runtime_transition_test: %s\n", message);
        return false;
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    bool ok = true;

    {
        NekoGui_Runtime::TransitionCoordinator coordinator;
        const auto start = coordinator.TryBegin(NekoGui_Runtime::TransitionKind::Start);
        ok &= expect(start.valid, "first transition must acquire ownership");
        ok &= expect(coordinator.IsCurrent(start), "first ticket must be current");
        ok &= expect(!coordinator.TryBegin(NekoGui_Runtime::TransitionKind::Stop).valid,
                     "a second transition must not overlap");

        std::atomic_bool completed = false;
        std::thread worker([&] {
            completed = coordinator.CompleteOrHandoff(start).completed;
        });
        worker.join();
        ok &= expect(completed.load(), "a worker may complete its own generation safely");
        ok &= expect(!coordinator.Busy(), "completion must release the coordinator");
    }

    {
        NekoGui_Runtime::TransitionCoordinator coordinator;
        const auto oldStart = coordinator.TryBegin(NekoGui_Runtime::TransitionKind::Start);
        ok &= expect(coordinator.CompleteOrHandoff(oldStart).completed,
                     "the old generation must drain first");
        const auto newStop = coordinator.TryBegin(NekoGui_Runtime::TransitionKind::Stop);
        ok &= expect(newStop.valid, "a new generation must start after the old one drains");
        ok &= expect(!coordinator.CompleteOrHandoff(oldStart).completed,
                      "a stale completion must not release a newer transition");
        ok &= expect(coordinator.IsCurrent(newStop),
                      "the newer transition must remain owned after stale completion");
        ok &= expect(coordinator.CompleteOrHandoff(newStop).completed,
                     "the current generation must complete");
    }

    {
        NekoGui_Runtime::TransitionCoordinator coordinator;
        std::atomic_int depthGate = 0;
        const auto oldStart = coordinator.TryBegin(
            NekoGui_Runtime::TransitionKind::Start,
            &depthGate);
        ok &= expect(oldStart.valid && depthGate.load() == 1,
                     "acquiring a transition must raise the mutation-depth gate");

        const auto rejectedStop = coordinator.TryBegin(
            NekoGui_Runtime::TransitionKind::Stop,
            &depthGate);
        ok &= expect(!rejectedStop.valid && depthGate.load() == 1,
                     "a rejected acquisition must retain the active owner's gate");

        ok &= expect(coordinator.CompleteOrHandoff(oldStart, &depthGate).completed &&
                         depthGate.load() == 0,
                     "the current owner must clear the gate only when quiescent");
        const auto newStop = coordinator.TryBegin(
            NekoGui_Runtime::TransitionKind::Stop,
            &depthGate);
        ok &= expect(newStop.valid && depthGate.load() == 1,
                     "a newer transition must acquire ownership and the gate together");
        ok &= expect(!coordinator.CompleteOrHandoff(oldStart, &depthGate).completed &&
                         depthGate.load() == 1,
                     "a stale completion must not clear a newer owner's gate");
        ok &= expect(coordinator.CompleteOrHandoff(newStop, &depthGate).completed &&
                         depthGate.load() == 0,
                     "the newer owner must release the gate on completion");

        coordinator.RequestCrashCleanup(&depthGate);
        ok &= expect(depthGate.load() == 1,
                     "pending crash cleanup must raise the mutation-depth gate");
        ok &= expect(!coordinator.TryBegin(
                          NekoGui_Runtime::TransitionKind::Start,
                          &depthGate).valid &&
                         depthGate.load() == 1,
                     "a failed normal begin must retain a pending cleanup gate");
        const auto cleanup = coordinator.TryBegin(
            NekoGui_Runtime::TransitionKind::CrashCleanup,
            &depthGate);
        ok &= expect(cleanup.valid && depthGate.load() == 1,
                     "crash cleanup must take ownership without dropping the gate");
        ok &= expect(coordinator.CompleteOrHandoff(cleanup, &depthGate).completed &&
                         depthGate.load() == 0,
                     "completed crash cleanup must clear the gate when quiescent");
        ok &= expect(!coordinator.TryBegin(
                          NekoGui_Runtime::TransitionKind::CrashCleanup,
                          &depthGate).valid &&
                         depthGate.load() == 0,
                     "an invalid idle cleanup begin must leave a quiescent gate clear");
    }

    {
        NekoGui_Runtime::TransitionCoordinator coordinator;
        const auto start = coordinator.TryBegin(NekoGui_Runtime::TransitionKind::Start);
        coordinator.RequestCrashCleanup();
        ok &= expect(!coordinator.TryBegin(NekoGui_Runtime::TransitionKind::Stop).valid,
                     "a pending crash cleanup must fence normal transitions");
        const auto completion = coordinator.CompleteOrHandoff(start);
        ok &= expect(completion.completed && completion.handoff.valid,
                     "completion must atomically hand ownership to crash cleanup");
        ok &= expect(
            completion.handoff.kind == NekoGui_Runtime::TransitionKind::CrashCleanup &&
                coordinator.IsCurrent(completion.handoff),
            "crash cleanup handoff must remain continuously owned");
        ok &= expect(coordinator.Busy(), "handoff must not expose an idle coordinator");
        ok &= expect(coordinator.CompleteOrHandoff(completion.handoff).completed,
                     "crash cleanup must complete its own generation");
    }

    {
        NekoGui_Runtime::TransitionCoordinator coordinator;
        coordinator.RequestCrashCleanup();
        const auto cleanup = coordinator.TryBegin(NekoGui_Runtime::TransitionKind::CrashCleanup);
        ok &= expect(cleanup.valid, "idle pending crash cleanup must acquire ownership");
        coordinator.RequestCrashCleanup();
        const auto completion = coordinator.CompleteOrHandoff(cleanup);
        ok &= expect(completion.handoff.valid &&
                         completion.handoff.generation > cleanup.generation,
                     "a crash arriving during cleanup must hand off to a newer cleanup generation");
        ok &= expect(coordinator.CompleteOrHandoff(completion.handoff).completed,
                     "the chained crash cleanup must drain");
    }

    {
        NekoGui_Runtime::DaemonGenerationState daemon;
        (void) daemon.QueueProfileForNextStart(17);
        const auto firstGeneration = daemon.BeginProcessStart("daemon-1");
        const auto firstInstance = daemon.CurrentInstance();
        ok &= expect(firstInstance.valid() && !firstInstance.ready &&
                         firstInstance.generation == firstGeneration &&
                         firstInstance.instanceId == "daemon-1",
                     "daemon start must atomically publish its generation and identity");
        ok &= expect(!daemon.MarkProcessReady(firstGeneration, "wrong-daemon").accepted &&
                         !daemon.IsReady(firstGeneration),
                     "a mismatched handshake must not mark the daemon ready");
        const auto oldCrashToken = daemon.CrashRestartToken();
        const auto firstConfirmation = daemon.MarkProcessReady(firstGeneration, "daemon-1");
        const auto firstReady = firstConfirmation.profileStart;
        ok &= expect(firstConfirmation.accepted,
                     "the exact daemon handshake must be accepted");
        ok &= expect(firstReady.valid && firstReady.profileId == 17 &&
                         firstReady.daemonGeneration == firstGeneration,
                     "queued profile must emit a generation-bound ready request");
        ok &= expect(daemon.ConsumeReadyProfile(firstReady),
                     "a current ready request must be consumed exactly once");
        ok &= expect(!daemon.ConsumeReadyProfile(firstReady),
                     "a ready request must not be consumed twice");

        ok &= expect(daemon.MarkProcessStopped() == firstGeneration,
                     "the first daemon must stop before queueing for its replacement");
        ok &= expect(!daemon.CurrentInstance().valid() &&
                         !daemon.MarkProcessReady(firstGeneration, "daemon-1").accepted,
                     "a late handshake must not revive a stopped daemon identity");
        (void) daemon.QueueProfileForNextStart(23);
        const auto secondGeneration = daemon.BeginProcessStart("daemon-2");
        ok &= expect(secondGeneration > firstGeneration, "daemon generation must be monotonic");
        ok &= expect(!daemon.CanRunCrashRestart(oldCrashToken),
                      "an old crash timer must not restart a newer daemon");
        ok &= expect(daemon.CancelQueuedProfile(), "explicit stop must cancel a queued profile");
        ok &= expect(!daemon.MarkProcessReady(secondGeneration, "daemon-2").profileStart.valid,
                      "a cancelled profile must not start when the daemon becomes ready");

        const auto emittedThenCancelled = daemon.QueueProfileForNextStart(31);
        ok &= expect(emittedThenCancelled.valid,
                     "a profile queued for a ready daemon must emit a request");
        ok &= expect(daemon.CancelQueuedProfile(),
                     "explicit stop must also cancel an emitted-but-unconsumed request");
        ok &= expect(!daemon.ConsumeReadyProfile(emittedThenCancelled),
                     "an event delivered after explicit stop must be stale");

        const auto supersededEvent = daemon.QueueProfileForNextStart(37);
        const auto replacementEvent = daemon.QueueProfileForNextStart(38);
        ok &= expect(!daemon.ConsumeReadyProfile(supersededEvent) &&
                         daemon.ConsumeReadyProfile(replacementEvent),
                     "a newer queued request must supersede an older emitted event");

        const auto oldDaemonEvent = daemon.QueueProfileForNextStart(41);
        ok &= expect(oldDaemonEvent.valid, "old daemon must emit its queued request");
        const auto thirdGeneration = daemon.BeginProcessStart("daemon-3");
        ok &= expect(!daemon.ConsumeReadyProfile(oldDaemonEvent),
                     "a ready event from an older daemon must not reach a newer daemon");
        ok &= expect(!daemon.IsReady(thirdGeneration),
                     "a newly starting daemon must not be reported ready");
        const auto reboundEvent = daemon.MarkProcessReady(thirdGeneration, "daemon-3").profileStart;
        ok &= expect(!reboundEvent.valid,
                     "an already-emitted old request must not be rebound to a replacement daemon");
    }

    {
        NekoGui_Runtime::DaemonGenerationState daemon;
        const auto generation = daemon.BeginProcessStart("daemon-ready-first");
        ok &= expect(!daemon.MarkProcessReady(generation, "daemon-ready-first").profileStart.valid && daemon.IsReady(generation),
                     "a daemon may become ready before a profile is queued");
        const auto immediate = daemon.QueueProfileForNextStart(47);
        ok &= expect(immediate.valid && immediate.daemonGeneration == generation &&
                         immediate.profileId == 47,
                     "queueing against an already-ready daemon must return an immediate request");
        ok &= expect(daemon.ConsumeReadyProfile(immediate),
                     "an immediate ready request must retain normal one-shot consumption");
    }

    {
        NekoGui_Runtime::DaemonGenerationState daemon;
        const auto generation = daemon.BeginProcessStart("daemon-concurrent");
        NekoGui_Runtime::DaemonProfileStartRequest fromReady;
        NekoGui_Runtime::DaemonProfileStartRequest fromQueue;
        std::atomic_int rendezvous = 0;
        auto meet = [&] {
            rendezvous.fetch_add(1);
            while (rendezvous.load() < 2) std::this_thread::yield();
        };
        std::thread readyThread([&] {
            meet();
            fromReady = daemon.MarkProcessReady(generation, "daemon-concurrent").profileStart;
        });
        std::thread queueThread([&] {
            meet();
            fromQueue = daemon.QueueProfileForNextStart(49);
        });
        readyThread.join();
        queueThread.join();

        const auto validCount = static_cast<int>(fromReady.valid) +
                                static_cast<int>(fromQueue.valid);
        const auto request = fromReady.valid ? fromReady : fromQueue;
        ok &= expect(validCount == 1 && request.daemonGeneration == generation &&
                         request.profileId == 49,
                     "concurrent ready/queue must emit exactly one generation-bound request");
        ok &= expect(daemon.ConsumeReadyProfile(request) &&
                         !daemon.ConsumeReadyProfile(request),
                     "the concurrent ready/queue request must remain one-shot");
    }

    {
        NekoGui_Runtime::DaemonGenerationState daemon;
        (void) daemon.QueueProfileForNextStart(53);
        const auto generation = daemon.BeginProcessStart("daemon-cancel");
        const auto request = daemon.MarkProcessReady(generation, "daemon-cancel").profileStart;
        std::atomic_bool cancelled = false;
        std::atomic_bool consumed = false;
        std::thread cancelThread([&] { cancelled = daemon.CancelQueuedProfile(); });
        std::thread consumeThread([&] { consumed = daemon.ConsumeReadyProfile(request); });
        cancelThread.join();
        consumeThread.join();
        ok &= expect(cancelled.load() != consumed.load(),
                     "cancel and consume must serialize with exactly one winner");
        ok &= expect(daemon.MarkProcessStopped() == generation && !daemon.IsReady(generation),
                     "process stop must invalidate readiness for its generation");
    }

    {
        using namespace std::chrono_literals;
        NekoGui_Runtime::DaemonProcessExitState processExit;
        const NekoGui_Runtime::DaemonProcessSnapshot first{
            7,
            "daemon-exit-1",
            41001,
        };
        const NekoGui_Runtime::DaemonProcessSnapshot wrongPid{
            first.generation,
            first.instanceId,
            first.processId + 1,
        };
        ok &= expect(processExit.MarkProcessStarted(first),
                     "a valid daemon process identity must arm the finished fence");
        ok &= expect(!processExit.MarkProcessStarted(first),
                     "a second process must not replace a still-running finished fence");
        ok &= expect(processExit.CurrentProcess().processId == first.processId,
                     "the armed finished fence must expose the exact PID");

        std::atomic_bool waiterObserved = false;
        NekoGui_Runtime::DaemonProcessFinishedResult waitedResult;
        std::thread waiter([&] {
            waiterObserved = processExit.WaitForFinished(first, 500ms, &waitedResult);
        });
        const auto firstFinished = processExit.MarkProcessFinished(0, true);
        waiter.join();
        ok &= expect(firstFinished.valid && waiterObserved.load() && waitedResult.valid &&
                         waitedResult.normalExit && waitedResult.exitCode == 0 &&
                         waitedResult.process.generation == first.generation &&
                         waitedResult.process.instanceId == first.instanceId &&
                         waitedResult.process.processId == first.processId,
                     "the exact generation/UUID/PID NormalExit must release the waiter");
        ok &= expect(!processExit.WaitForFinished(wrongPid, 0ms),
                     "a PID mismatch must not consume another process finished event");

        const NekoGui_Runtime::DaemonProcessSnapshot second{
            8,
            "daemon-exit-2",
            41002,
        };
        ok &= expect(processExit.MarkProcessStarted(second),
                     "a new process may arm only after the prior process finished");
        const auto secondFinished = processExit.MarkProcessFinished(9, false);
        NekoGui_Runtime::DaemonProcessFinishedResult finishedBeforeWait;
        ok &= expect(secondFinished.valid &&
                         processExit.WaitForFinished(second, 0ms, &finishedBeforeWait) &&
                         !finishedBeforeWait.normalExit && finishedBeforeWait.exitCode == 9,
                     "finished-before-wait must remain observable without becoming a clean exit");
        ok &= expect(!processExit.MarkProcessFinished(0, true).valid,
                     "a duplicate finished signal must not create another process result");
    }

    return ok ? 0 : 1;
}
