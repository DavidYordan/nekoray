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
	err := lifecycle.start(1, func() (*managedCore, error) {
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
	err := lifecycle.start(1, func() (*managedCore, error) {
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
	err = lifecycle.start(2, func() (*managedCore, error) {
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
	if err := lifecycle.start(1, func() (*managedCore, error) { return fake.candidate(), nil }); err != nil {
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
	if err := lifecycle.start(1, func() (*managedCore, error) { return first.candidate(), nil }); err != nil {
		t.Fatalf("first start failed: %v", err)
	}
	oldReference := lifecycle.currentReference()
	oldClient := generationBoundHTTPClient(oldReference)
	if err := lifecycle.stop(2); err != nil {
		t.Fatalf("first stop failed: %v", err)
	}

	second := &fakeManagedCore{}
	if err := lifecycle.start(3, func() (*managedCore, error) { return second.candidate(), nil }); err != nil {
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
	if err := lifecycle.start(1, func() (*managedCore, error) { return started.candidate(), nil }); err != nil {
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
	if err := lifecycle.start(1, func() (*managedCore, error) { return fake.candidate(), nil }); err != nil {
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
		firstDone <- lifecycle.start(1, func() (*managedCore, error) {
			close(firstFactoryEntered)
			<-releaseFirstFactory
			return first.candidate(), nil
		})
	}()
	<-firstFactoryEntered

	var secondFactoryCalls atomic.Int32
	secondDone := make(chan error, 1)
	go func() {
		secondDone <- lifecycle.start(2, func() (*managedCore, error) {
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
	err := lifecycle.start(1, func() (*managedCore, error) {
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

func TestDialHoldsLifecycleMutexAgainstStop(t *testing.T) {
	lifecycle := newCoreLifecycle()
	fake := &fakeManagedCore{
		dialEnter:  make(chan struct{}),
		dialWait:   make(chan struct{}),
		closeEnter: make(chan struct{}),
	}
	if err := lifecycle.start(1, func() (*managedCore, error) { return fake.candidate(), nil }); err != nil {
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
	lifecycle := newCoreLifecycle()
	exitCalls := 0
	fakeExit := func(code int) {
		if code != 0 {
			t.Fatalf("unexpected exit code %d", code)
		}
		exitCalls++
	}

	if err := lifecycle.exitWhenStopped(1, fakeExit); !errors.Is(err, errProcessExitReturned) {
		t.Fatalf("stopped exit returned %v", err)
	}
	if exitCalls != 1 {
		t.Fatalf("stopped lifecycle invoked exit %d time(s)", exitCalls)
	}

	fake := &fakeManagedCore{}
	if err := lifecycle.start(2, func() (*managedCore, error) { return fake.candidate(), nil }); err != nil {
		t.Fatalf("start failed: %v", err)
	}
	if err := lifecycle.exitWhenStopped(3, fakeExit); !errors.Is(err, errCoreExitRequiresStop) {
		t.Fatalf("active exit returned %v", err)
	}
	if exitCalls != 1 || fake.cancelCount.Load() != 0 || fake.closeCount.Load() != 0 {
		t.Fatalf("active Exit changed lifecycle: exits=%d cancel=%d close=%d", exitCalls, fake.cancelCount.Load(), fake.closeCount.Load())
	}

	fake.closeErr = errFakeClose
	if err := lifecycle.stop(4); !errors.Is(err, errCoreLifecycleBlocked) {
		t.Fatalf("stop did not block: %v", err)
	}
	if err := lifecycle.exitWhenStopped(5, fakeExit); !errors.Is(err, errCoreLifecycleBlocked) {
		t.Fatalf("blocked exit returned %v", err)
	}
	if exitCalls != 1 {
		t.Fatalf("blocked lifecycle invoked exit %d time(s)", exitCalls)
	}
}
