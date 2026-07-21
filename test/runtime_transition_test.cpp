#include "main/RuntimeTransition.hpp"

#include <QCoreApplication>
#include <QDebug>

#include <atomic>
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
        const auto firstGeneration = daemon.BeginProcessStart();
        const auto oldCrashToken = daemon.CrashRestartToken();
        const auto firstReady = daemon.MarkProcessReady();
        ok &= expect(firstReady.valid && firstReady.profileId == 17 &&
                         firstReady.daemonGeneration == firstGeneration,
                     "queued profile must emit a generation-bound ready request");
        ok &= expect(daemon.ConsumeReadyProfile(firstReady),
                     "a current ready request must be consumed exactly once");
        ok &= expect(!daemon.ConsumeReadyProfile(firstReady),
                     "a ready request must not be consumed twice");

        ok &= expect(daemon.MarkProcessStopped() == firstGeneration,
                     "the first daemon must stop before queueing for its replacement");
        (void) daemon.QueueProfileForNextStart(23);
        const auto secondGeneration = daemon.BeginProcessStart();
        ok &= expect(secondGeneration > firstGeneration, "daemon generation must be monotonic");
        ok &= expect(!daemon.CanRunCrashRestart(oldCrashToken),
                      "an old crash timer must not restart a newer daemon");
        ok &= expect(daemon.CancelQueuedProfile(), "explicit stop must cancel a queued profile");
        ok &= expect(!daemon.MarkProcessReady().valid,
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
        const auto thirdGeneration = daemon.BeginProcessStart();
        ok &= expect(!daemon.ConsumeReadyProfile(oldDaemonEvent),
                     "a ready event from an older daemon must not reach a newer daemon");
        ok &= expect(!daemon.IsReady(thirdGeneration),
                     "a newly starting daemon must not be reported ready");
        const auto reboundEvent = daemon.MarkProcessReady();
        ok &= expect(!reboundEvent.valid,
                     "an already-emitted old request must not be rebound to a replacement daemon");
    }

    {
        NekoGui_Runtime::DaemonGenerationState daemon;
        const auto generation = daemon.BeginProcessStart();
        ok &= expect(!daemon.MarkProcessReady().valid && daemon.IsReady(generation),
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
        const auto generation = daemon.BeginProcessStart();
        NekoGui_Runtime::DaemonProfileStartRequest fromReady;
        NekoGui_Runtime::DaemonProfileStartRequest fromQueue;
        std::atomic_int rendezvous = 0;
        auto meet = [&] {
            rendezvous.fetch_add(1);
            while (rendezvous.load() < 2) std::this_thread::yield();
        };
        std::thread readyThread([&] {
            meet();
            fromReady = daemon.MarkProcessReady();
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
        const auto generation = daemon.BeginProcessStart();
        const auto request = daemon.MarkProcessReady();
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

    return ok ? 0 : 1;
}
