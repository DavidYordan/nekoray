package main

import (
	"context"
	"errors"
	"net"
	"sync/atomic"
	"testing"
	"time"

	"nekobox_core/internal/boxapi"
)

var (
	errFakeCreate = errors.New("fake candidate creation failed")
	errFakeClose  = errors.New("fake close failed")
	errFakeDial   = errors.New("fake dial reached")
)

type fakeManagedCore struct {
	cancelCount atomic.Int32
	closeCount  atomic.Int32
	dialCount   atomic.Int32
	queryCount  atomic.Int32
	closeErr    error
	queryValue  int64
	dialEnter   chan struct{}
	dialWait    chan struct{}
	queryEnter  chan struct{}
	queryWait   chan struct{}
	closeEnter  chan struct{}
	closeWait   chan struct{}
}

func (f *fakeManagedCore) candidate() *managedCore {
	return &managedCore{
		cancel: func() {
			f.cancelCount.Add(1)
		},
		close: func() error {
			f.closeCount.Add(1)
			if f.closeEnter != nil {
				select {
				case <-f.closeEnter:
				default:
					close(f.closeEnter)
				}
			}
			if f.closeWait != nil {
				<-f.closeWait
			}
			return f.closeErr
		},
		dial: func(context.Context, string, string) (net.Conn, error) {
			f.dialCount.Add(1)
			if f.dialEnter != nil {
				select {
				case <-f.dialEnter:
				default:
					close(f.dialEnter)
				}
			}
			if f.dialWait != nil {
				<-f.dialWait
			}
			return nil, errFakeDial
		},
		queryStats: func(context.Context, string, bool) (int64, error) {
			f.queryCount.Add(1)
			if f.queryEnter != nil {
				select {
				case <-f.queryEnter:
				default:
					close(f.queryEnter)
				}
			}
			if f.queryWait != nil {
				<-f.queryWait
			}
			return f.queryValue, nil
		},
	}
}

func TestLifecycleDoesNotPublishFailedCandidate(t *testing.T) {
	lifecycle := newCoreLifecycle()
	fake := &fakeManagedCore{}
	err := lifecycle.start(1, "", func() (*managedCore, error) {
		return fake.candidate(), errFakeCreate
	})
	if !errors.Is(err, errFakeCreate) {
		t.Fatalf("expected candidate error, got %v", err)
	}
	phase, _, hasCurrent := lifecycle.snapshot()
	if phase != coreLifecycleStopped || hasCurrent {
		t.Fatalf("failed candidate was published: phase=%v current=%v", phase, hasCurrent)
	}
	if fake.cancelCount.Load() != 1 || fake.closeCount.Load() != 1 {
		t.Fatalf("failed candidate cleanup mismatch: cancel=%d close=%d", fake.cancelCount.Load(), fake.closeCount.Load())
	}

	reference := lifecycle.currentReference()
	_, dialErr := lifecycle.dial(context.Background(), reference, "tcp", "unit.test:443")
	if !errors.Is(dialErr, boxapi.ErrNoActiveInstance) {
		t.Fatalf("failed candidate became dialable: %v", dialErr)
	}
	if fake.dialCount.Load() != 0 {
		t.Fatalf("failed candidate dial was invoked %d time(s)", fake.dialCount.Load())
	}
}

func TestCandidateCleanupFailureBlocksLifecycle(t *testing.T) {
	lifecycle := newCoreLifecycle()
	fake := &fakeManagedCore{closeErr: errFakeClose}
	err := lifecycle.start(1, "", func() (*managedCore, error) {
		return fake.candidate(), errFakeCreate
	})
	if !errors.Is(err, errCoreLifecycleBlocked) || !errors.Is(err, errFakeClose) {
		t.Fatalf("expected blocked candidate cleanup error, got %v", err)
	}
	phase, _, hasCurrent := lifecycle.snapshot()
	if phase != coreLifecycleBlocked || !hasCurrent {
		t.Fatalf("cleanup failure was not retained: phase=%v current=%v", phase, hasCurrent)
	}
	if fake.cancelCount.Load() != 1 || fake.closeCount.Load() != 1 {
		t.Fatalf("candidate cleanup mismatch: cancel=%d close=%d", fake.cancelCount.Load(), fake.closeCount.Load())
	}

	factoryCalled := false
	err = lifecycle.start(2, "", func() (*managedCore, error) {
		factoryCalled = true
		return (&fakeManagedCore{}).candidate(), nil
	})
	if !errors.Is(err, errCoreLifecycleBlocked) || factoryCalled {
		t.Fatalf("blocked lifecycle accepted another candidate: err=%v called=%v", err, factoryCalled)
	}
}

func TestStopCloseFailureRetainsBlockedState(t *testing.T) {
	lifecycle := newCoreLifecycle()
	fake := &fakeManagedCore{closeErr: errFakeClose}
	if err := lifecycle.start(1, "", func() (*managedCore, error) { return fake.candidate(), nil }); err != nil {
		t.Fatalf("start failed: %v", err)
	}
	reference := lifecycle.currentReference()
	client := generationBoundHTTPClient(reference)

	stopErr := lifecycle.stop(2)
	if !errors.Is(stopErr, errCoreLifecycleBlocked) || !errors.Is(stopErr, errFakeClose) {
		t.Fatalf("expected blocked close error, got %v", stopErr)
	}
	phase, _, hasCurrent := lifecycle.snapshot()
	if phase != coreLifecycleBlocked || !hasCurrent {
		t.Fatalf("close failure was not retained: phase=%v current=%v", phase, hasCurrent)
	}
	if fake.cancelCount.Load() != 1 || fake.closeCount.Load() != 1 {
		t.Fatalf("stop count mismatch: cancel=%d close=%d", fake.cancelCount.Load(), fake.closeCount.Load())
	}

	if _, err := lifecycle.dial(context.Background(), reference, "tcp", "unit.test:443"); !errors.Is(err, errCoreLifecycleBlocked) {
		t.Fatalf("blocked lifecycle dial returned %v", err)
	}
	if _, err := lifecycle.queryStats(context.Background(), "outbound>>>proxy>>>traffic>>>uplink", true); !errors.Is(err, errCoreLifecycleBlocked) {
		t.Fatalf("blocked lifecycle stats returned %v", err)
	}
	if _, err := client.Get("http://unit.test/"); !errors.Is(err, errCoreLifecycleBlocked) {
		t.Fatalf("old HTTP client did not fail closed in blocked state: %v", err)
	}
	if fake.dialCount.Load() != 0 || fake.queryCount.Load() != 0 {
		t.Fatalf("blocked resource was used: dial=%d query=%d", fake.dialCount.Load(), fake.queryCount.Load())
	}

	if err := lifecycle.stop(3); !errors.Is(err, errCoreLifecycleBlocked) {
		t.Fatalf("blocked stop retry returned %v", err)
	}
	if fake.closeCount.Load() != 1 {
		t.Fatalf("blocked lifecycle retried an indeterminate close %d time(s)", fake.closeCount.Load())
	}
}

func TestOldReferenceAndHTTPClientCannotUseNewGeneration(t *testing.T) {
	lifecycle := newCoreLifecycle()
	first := &fakeManagedCore{}
	if err := lifecycle.start(1, "", func() (*managedCore, error) { return first.candidate(), nil }); err != nil {
		t.Fatalf("first start failed: %v", err)
	}
	oldReference := lifecycle.currentReference()
	oldClient := generationBoundHTTPClient(oldReference)
	if err := lifecycle.stop(2); err != nil {
		t.Fatalf("first stop failed: %v", err)
	}

	second := &fakeManagedCore{}
	if err := lifecycle.start(3, "", func() (*managedCore, error) { return second.candidate(), nil }); err != nil {
		t.Fatalf("second start failed: %v", err)
	}
	if _, err := lifecycle.dial(context.Background(), oldReference, "tcp", "unit.test:443"); !errors.Is(err, errStaleCoreGeneration) {
		t.Fatalf("old reference returned %v", err)
	}
	if _, err := oldClient.Get("http://unit.test/"); !errors.Is(err, errStaleCoreGeneration) {
		t.Fatalf("old HTTP client returned %v", err)
	}
	if first.dialCount.Load() != 0 || second.dialCount.Load() != 0 {
		t.Fatalf("stale reference reached a resource: first=%d second=%d", first.dialCount.Load(), second.dialCount.Load())
	}
}

func TestStoppedReferenceCannotBindToLaterGeneration(t *testing.T) {
	lifecycle := newCoreLifecycle()
	stoppedReference := lifecycle.currentReference()
	client := generationBoundHTTPClient(stoppedReference)
	started := &fakeManagedCore{}
	if err := lifecycle.start(1, "", func() (*managedCore, error) { return started.candidate(), nil }); err != nil {
		t.Fatalf("start failed: %v", err)
	}
	if _, err := client.Get("http://unit.test/"); !errors.Is(err, errStaleCoreGeneration) {
		t.Fatalf("stopped reference rebound to a later generation: %v", err)
	}
	if started.dialCount.Load() != 0 {
		t.Fatalf("stopped reference reached later generation %d time(s)", started.dialCount.Load())
	}
}

func TestQueryStatsHoldsLifecycleMutexAgainstStop(t *testing.T) {
	lifecycle := newCoreLifecycle()
	fake := &fakeManagedCore{
		queryValue: 19,
		queryEnter: make(chan struct{}),
		queryWait:  make(chan struct{}),
		closeEnter: make(chan struct{}),
	}
	if err := lifecycle.start(1, "", func() (*managedCore, error) { return fake.candidate(), nil }); err != nil {
		t.Fatalf("start failed: %v", err)
	}

	queryDone := make(chan error, 1)
	go func() {
		value, err := lifecycle.queryStats(context.Background(), "counter", true)
		if err == nil && value != fake.queryValue {
			err = errors.New("unexpected query result")
		}
		queryDone <- err
	}()
	<-fake.queryEnter

	stopStarted := make(chan struct{})
	stopDone := make(chan error, 1)
	go func() {
		close(stopStarted)
		stopDone <- lifecycle.stop(2)
	}()
	<-stopStarted
	select {
	case <-fake.closeEnter:
		t.Fatal("Stop entered Close while QueryStats still owned the lifecycle mutex")
	case <-time.After(25 * time.Millisecond):
	}

	close(fake.queryWait)
	if err := <-queryDone; err != nil {
		t.Fatalf("query failed: %v", err)
	}
	if err := <-stopDone; err != nil {
		t.Fatalf("stop failed: %v", err)
	}
	select {
	case <-fake.closeEnter:
	default:
		t.Fatal("Stop did not close after QueryStats released the lifecycle mutex")
	}
}

func TestConcurrentStartPublishesOnce(t *testing.T) {
	lifecycle := newCoreLifecycle()
	first := &fakeManagedCore{}
	firstFactoryEntered := make(chan struct{})
	releaseFirstFactory := make(chan struct{})
	firstDone := make(chan error, 1)
	go func() {
		firstDone <- lifecycle.start(1, "", func() (*managedCore, error) {
			close(firstFactoryEntered)
			<-releaseFirstFactory
			return first.candidate(), nil
		})
	}()
	<-firstFactoryEntered

	var secondFactoryCalls atomic.Int32
	secondDone := make(chan error, 1)
	go func() {
		secondDone <- lifecycle.start(2, "", func() (*managedCore, error) {
			secondFactoryCalls.Add(1)
			return (&fakeManagedCore{}).candidate(), nil
		})
	}()
	select {
	case err := <-secondDone:
		t.Fatalf("second Start escaped lifecycle serialization before candidate publication: %v", err)
	case <-time.After(25 * time.Millisecond):
	}

	close(releaseFirstFactory)
	if err := <-firstDone; err != nil {
		t.Fatalf("first start failed: %v", err)
	}
	if err := <-secondDone; !errors.Is(err, errCoreAlreadyStarted) {
		t.Fatalf("second start returned %v", err)
	}
	if secondFactoryCalls.Load() != 0 {
		t.Fatalf("second candidate factory ran %d time(s)", secondFactoryCalls.Load())
	}
}

func TestNewerStopSupersedesDelayedOlderStart(t *testing.T) {
	lifecycle := newCoreLifecycle()
	if err := lifecycle.stop(2); err != nil {
		t.Fatalf("newer stopped-state command failed: %v", err)
	}

	factoryCalled := false
	err := lifecycle.start(1, "", func() (*managedCore, error) {
		factoryCalled = true
		return (&fakeManagedCore{}).candidate(), nil
	})
	if !errors.Is(err, errStaleCommandSequence) {
		t.Fatalf("delayed older Start returned %v", err)
	}
	phase, _, hasCurrent := lifecycle.snapshot()
	if factoryCalled || phase != coreLifecycleStopped || hasCurrent {
		t.Fatalf(
			"delayed older Start changed lifecycle: factory=%v phase=%v current=%v",
			factoryCalled,
			phase,
			hasCurrent,
		)
	}
}

func TestReconcileBarrierFencesDelayedStart(t *testing.T) {
	lifecycle := newCoreLifecycle()
	const configHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	snapshot, err := lifecycle.reconcile(
		2,
		1,
		coreLifecycleCommandStart,
		configHash,
	)
	if err != nil {
		t.Fatalf("reconcile stopped lifecycle: %v", err)
	}
	if snapshot.phase != coreLifecycleStopped || snapshot.hasCurrent ||
		snapshot.orderingWatermark != 2 ||
		snapshot.targetCommand.outcome != coreLifecycleOutcomeFencedNotAdmitted {
		t.Fatalf("unexpected fenced snapshot: %#v", snapshot)
	}

	factoryCalled := false
	err = lifecycle.start(1, configHash, func() (*managedCore, error) {
		factoryCalled = true
		return (&fakeManagedCore{}).candidate(), nil
	})
	if !errors.Is(err, errStaleCommandSequence) || factoryCalled {
		t.Fatalf("delayed Start crossed reconcile barrier: err=%v factory=%v", err, factoryCalled)
	}
}

func TestReconcileBarrierFencesDelayedExit(t *testing.T) {
	lifecycle := newCoreLifecycle()
	snapshot, err := lifecycle.reconcile(
		2,
		1,
		coreLifecycleCommandExit,
		"",
	)
	if err != nil {
		t.Fatalf("reconcile stopped lifecycle: %v", err)
	}
	if snapshot.phase != coreLifecycleStopped || snapshot.hasCurrent ||
		snapshot.orderingWatermark != 2 || snapshot.activeStartCommandSequence != 0 ||
		snapshot.currentConfigSHA256 != "" ||
		snapshot.targetCommand.sequence != 1 ||
		snapshot.targetCommand.kind != coreLifecycleCommandExit ||
		snapshot.targetCommand.outcome != coreLifecycleOutcomeFencedNotAdmitted ||
		snapshot.targetCommand.configSHA256 != "" || snapshot.targetConfigMatches {
		t.Fatalf("unexpected fenced Exit snapshot: %#v", snapshot)
	}

	// A lost reconciliation response may be retried with another higher
	// barrier while still naming the original Exit sequence. The proof remains
	// stable and the newest barrier becomes the ordering watermark.
	snapshot, err = lifecycle.reconcile(
		3,
		1,
		coreLifecycleCommandExit,
		"",
	)
	if err != nil {
		t.Fatalf("retry reconcile stopped lifecycle: %v", err)
	}
	if snapshot.phase != coreLifecycleStopped || snapshot.hasCurrent ||
		snapshot.orderingWatermark != 3 ||
		snapshot.targetCommand.outcome != coreLifecycleOutcomeFencedNotAdmitted {
		t.Fatalf("unexpected retried Exit fence: %#v", snapshot)
	}

	if _, err := lifecycle.requestExit(1); !errors.Is(err, errStaleCommandSequence) {
		t.Fatalf("delayed Exit crossed reconcile barrier: %v", err)
	}
	phase, _, hasCurrent := lifecycle.snapshot()
	if phase != coreLifecycleStopped || hasCurrent {
		t.Fatalf("delayed Exit changed lifecycle: phase=%v current=%v", phase, hasCurrent)
	}
}

func TestReconcileWaitsForStartAndReturnsExactOutcome(t *testing.T) {
	lifecycle := newCoreLifecycle()
	const configHash = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	factoryEntered := make(chan struct{})
	releaseFactory := make(chan struct{})
	startDone := make(chan error, 1)
	go func() {
		startDone <- lifecycle.start(1, configHash, func() (*managedCore, error) {
			close(factoryEntered)
			<-releaseFactory
			return (&fakeManagedCore{}).candidate(), nil
		})
	}()
	<-factoryEntered

	reconcileDone := make(chan coreLifecycleSnapshot, 1)
	reconcileErr := make(chan error, 1)
	go func() {
		snapshot, err := lifecycle.reconcile(2, 1, coreLifecycleCommandStart, configHash)
		reconcileDone <- snapshot
		reconcileErr <- err
	}()
	select {
	case <-reconcileDone:
		t.Fatal("reconcile escaped while Start still owned the lifecycle mutex")
	case <-time.After(25 * time.Millisecond):
	}

	close(releaseFactory)
	if err := <-startDone; err != nil {
		t.Fatalf("Start failed: %v", err)
	}
	snapshot := <-reconcileDone
	if err := <-reconcileErr; err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if snapshot.phase != coreLifecycleActive || !snapshot.hasCurrent ||
		snapshot.orderingWatermark != 2 ||
		snapshot.activeStartCommandSequence != 1 ||
		snapshot.currentConfigSHA256 != configHash ||
		snapshot.targetCommand.outcome != coreLifecycleOutcomeSucceeded ||
		!snapshot.targetConfigMatches {
		t.Fatalf("unexpected active reconciliation: %#v", snapshot)
	}
}

func TestReconcileDistinguishesCleanAndBlockedStartFailure(t *testing.T) {
	for _, test := range []struct {
		name        string
		closeErr    error
		wantPhase   coreLifecyclePhase
		wantOutcome coreLifecycleCommandOutcome
		wantCurrent bool
	}{
		{name: "failed-clean", wantPhase: coreLifecycleStopped, wantOutcome: coreLifecycleOutcomeFailedClean},
		{name: "blocked", closeErr: errFakeClose, wantPhase: coreLifecycleBlocked, wantOutcome: coreLifecycleOutcomeBlocked, wantCurrent: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			lifecycle := newCoreLifecycle()
			const configHash = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
			fake := &fakeManagedCore{closeErr: test.closeErr}
			startErr := lifecycle.start(1, configHash, func() (*managedCore, error) {
				return fake.candidate(), errFakeCreate
			})
			if !errors.Is(startErr, errFakeCreate) {
				t.Fatalf("start error: %v", startErr)
			}
			snapshot, err := lifecycle.reconcile(2, 1, coreLifecycleCommandStart, configHash)
			if err != nil {
				t.Fatalf("reconcile: %v", err)
			}
			if snapshot.phase != test.wantPhase ||
				snapshot.hasCurrent != test.wantCurrent ||
				snapshot.targetCommand.outcome != test.wantOutcome ||
				!snapshot.targetConfigMatches {
				t.Fatalf("unexpected failed Start reconciliation: %#v", snapshot)
			}
		})
	}
}

func TestReconcileWaitsForStopCloseOutcome(t *testing.T) {
	for _, test := range []struct {
		name        string
		closeErr    error
		wantPhase   coreLifecyclePhase
		wantOutcome coreLifecycleCommandOutcome
		wantCurrent bool
	}{
		{name: "stopped", wantPhase: coreLifecycleStopped, wantOutcome: coreLifecycleOutcomeSucceeded},
		{name: "blocked", closeErr: errFakeClose, wantPhase: coreLifecycleBlocked, wantOutcome: coreLifecycleOutcomeBlocked, wantCurrent: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			lifecycle := newCoreLifecycle()
			const configHash = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
			fake := &fakeManagedCore{
				closeErr:   test.closeErr,
				closeEnter: make(chan struct{}),
				closeWait:  make(chan struct{}),
			}
			if err := lifecycle.start(1, configHash, func() (*managedCore, error) {
				return fake.candidate(), nil
			}); err != nil {
				t.Fatalf("start: %v", err)
			}
			stopDone := make(chan error, 1)
			go func() { stopDone <- lifecycle.stop(2) }()
			<-fake.closeEnter

			reconcileDone := make(chan coreLifecycleSnapshot, 1)
			reconcileErr := make(chan error, 1)
			go func() {
				snapshot, err := lifecycle.reconcile(3, 2, coreLifecycleCommandStop, configHash)
				reconcileDone <- snapshot
				reconcileErr <- err
			}()
			select {
			case <-reconcileDone:
				t.Fatal("reconcile escaped while Stop still owned the lifecycle mutex")
			case <-time.After(25 * time.Millisecond):
			}

			close(fake.closeWait)
			stopErr := <-stopDone
			if test.closeErr == nil && stopErr != nil {
				t.Fatalf("stop: %v", stopErr)
			}
			if test.closeErr != nil && !errors.Is(stopErr, errCoreLifecycleBlocked) {
				t.Fatalf("blocked stop: %v", stopErr)
			}
			snapshot := <-reconcileDone
			if err := <-reconcileErr; err != nil {
				t.Fatalf("reconcile: %v", err)
			}
			if snapshot.phase != test.wantPhase ||
				snapshot.hasCurrent != test.wantCurrent ||
				snapshot.targetCommand.outcome != test.wantOutcome ||
				!snapshot.targetConfigMatches {
				t.Fatalf("unexpected Stop reconciliation: %#v", snapshot)
			}
		})
	}
}

func TestDialHoldsLifecycleMutexAgainstStop(t *testing.T) {
	lifecycle := newCoreLifecycle()
	fake := &fakeManagedCore{
		dialEnter:  make(chan struct{}),
		dialWait:   make(chan struct{}),
		closeEnter: make(chan struct{}),
	}
	if err := lifecycle.start(1, "", func() (*managedCore, error) { return fake.candidate(), nil }); err != nil {
		t.Fatalf("start failed: %v", err)
	}
	reference := lifecycle.currentReference()

	dialDone := make(chan error, 1)
	go func() {
		_, err := lifecycle.dial(context.Background(), reference, "tcp", "unit.test:443")
		dialDone <- err
	}()
	<-fake.dialEnter

	stopStarted := make(chan struct{})
	stopDone := make(chan error, 1)
	go func() {
		close(stopStarted)
		stopDone <- lifecycle.stop(2)
	}()
	<-stopStarted
	select {
	case <-fake.closeEnter:
		t.Fatal("Stop entered Close while Dial still owned the lifecycle mutex")
	case <-time.After(25 * time.Millisecond):
	}

	close(fake.dialWait)
	if err := <-dialDone; !errors.Is(err, errFakeDial) {
		t.Fatalf("dial returned %v", err)
	}
	if err := <-stopDone; err != nil {
		t.Fatalf("stop failed: %v", err)
	}
	select {
	case <-fake.closeEnter:
	default:
		t.Fatal("Stop did not close after Dial released the lifecycle mutex")
	}
}

func TestExitRequiresPreciselyStoppedLifecycle(t *testing.T) {
	t.Run("stopped becomes terminal exiting fence", func(t *testing.T) {
		lifecycle := newCoreLifecycle()
		snapshot, err := lifecycle.requestExit(1)
		if err != nil {
			t.Fatalf("request exit: %v", err)
		}
		if snapshot.phase != coreLifecycleExiting || snapshot.hasCurrent ||
			snapshot.orderingWatermark != 1 || snapshot.runtimeGeneration != 0 ||
			snapshot.targetCommand.sequence != 1 ||
			snapshot.targetCommand.kind != coreLifecycleCommandExit ||
			snapshot.targetCommand.outcome != coreLifecycleOutcomeSucceeded {
			t.Fatalf("unexpected Exit snapshot: %#v", snapshot)
		}

		factoryCalled := false
		if err := lifecycle.start(2, "", func() (*managedCore, error) {
			factoryCalled = true
			return (&fakeManagedCore{}).candidate(), nil
		}); !errors.Is(err, errCoreLifecycleExiting) {
			t.Fatalf("Start after Exit returned %v", err)
		}
		if factoryCalled {
			t.Fatal("Start factory ran after the Exit fence")
		}
		if err := lifecycle.stop(3); !errors.Is(err, errCoreLifecycleExiting) {
			t.Fatalf("Stop after Exit returned %v", err)
		}
		if _, err := lifecycle.reconcile(4, 1, coreLifecycleCommandExit, ""); !errors.Is(err, errCoreLifecycleExiting) {
			t.Fatalf("Reconcile after Exit returned %v", err)
		}
		if _, err := lifecycle.requestExit(5); !errors.Is(err, errCoreLifecycleExiting) {
			t.Fatalf("second Exit returned %v", err)
		}

		lifecycle.mu.Lock()
		watermark := lifecycle.lastCommandSequence
		lastCommand := lifecycle.lastLifecycleCommand
		lifecycle.mu.Unlock()
		if watermark != 1 || lastCommand.sequence != 1 ||
			lastCommand.kind != coreLifecycleCommandExit ||
			lastCommand.outcome != coreLifecycleOutcomeSucceeded {
			t.Fatalf("post-Exit command changed the terminal record: watermark=%d record=%#v", watermark, lastCommand)
		}
	})

	t.Run("active and blocked cores are never stopped implicitly", func(t *testing.T) {
		activeLifecycle := newCoreLifecycle()
		active := &fakeManagedCore{}
		if err := activeLifecycle.start(1, "", func() (*managedCore, error) { return active.candidate(), nil }); err != nil {
			t.Fatalf("start active lifecycle: %v", err)
		}
		if _, err := activeLifecycle.requestExit(2); !errors.Is(err, errCoreExitRequiresStop) {
			t.Fatalf("active Exit returned %v", err)
		}
		phase, _, hasCurrent := activeLifecycle.snapshot()
		if phase != coreLifecycleActive || !hasCurrent ||
			active.cancelCount.Load() != 0 || active.closeCount.Load() != 0 {
			t.Fatalf("active Exit changed lifecycle: phase=%v current=%v cancel=%d close=%d",
				phase, hasCurrent, active.cancelCount.Load(), active.closeCount.Load())
		}

		active.closeErr = errFakeClose
		if err := activeLifecycle.stop(3); !errors.Is(err, errCoreLifecycleBlocked) {
			t.Fatalf("stop did not block: %v", err)
		}
		if _, err := activeLifecycle.requestExit(4); !errors.Is(err, errCoreLifecycleBlocked) {
			t.Fatalf("blocked Exit returned %v", err)
		}
		if active.closeCount.Load() != 1 {
			t.Fatalf("blocked Exit retried Close %d time(s)", active.closeCount.Load())
		}
	})
}
