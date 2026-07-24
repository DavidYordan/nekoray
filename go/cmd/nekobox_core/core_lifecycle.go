package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"sync"

	"nekobox_core/internal/boxapi"
)

var (
	errCoreAlreadyStarted     = errors.New("core lifecycle already has an active instance")
	errCoreLifecycleBlocked   = errors.New("core lifecycle is blocked after an indeterminate shutdown")
	errCoreLifecycleBusy      = errors.New("core lifecycle transition is already in progress")
	errCoreLifecycleExiting   = errors.New("core lifecycle daemon exit is already committed")
	errInvalidCoreCandidate   = errors.New("invalid core lifecycle candidate")
	errStaleCoreGeneration    = errors.New("stale core generation reference")
	errForeignCoreReference   = errors.New("core generation reference belongs to another lifecycle owner")
	errCoreExitRequiresStop   = errors.New("core lifecycle must be stopped before daemon exit")
	errInvalidCommandSequence = errors.New("core lifecycle command sequence must be non-zero")
	errStaleCommandSequence   = errors.New("stale core lifecycle command sequence")
)

type coreLifecyclePhase uint8

const (
	coreLifecycleStopped coreLifecyclePhase = iota
	coreLifecycleStarting
	coreLifecycleActive
	coreLifecycleStopping
	coreLifecycleBlocked
	coreLifecycleExiting
)

type coreLifecycleCommandKind uint8

const (
	coreLifecycleCommandUnspecified coreLifecycleCommandKind = iota
	coreLifecycleCommandStart
	coreLifecycleCommandStop
	coreLifecycleCommandExit
)

type coreLifecycleCommandOutcome uint8

const (
	coreLifecycleOutcomeUnspecified coreLifecycleCommandOutcome = iota
	coreLifecycleOutcomeSucceeded
	coreLifecycleOutcomeFailedClean
	coreLifecycleOutcomeRejectedClean
	coreLifecycleOutcomeBlocked
	coreLifecycleOutcomeFencedNotAdmitted
	coreLifecycleOutcomeSupersededUnknown
)

type coreLifecycleCommandRecord struct {
	sequence     uint64
	kind         coreLifecycleCommandKind
	outcome      coreLifecycleCommandOutcome
	configSHA256 string
}

type coreLifecycleSnapshot struct {
	phase                      coreLifecyclePhase
	runtimeGeneration          uint64
	hasCurrent                 bool
	orderingWatermark          uint64
	activeStartCommandSequence uint64
	currentConfigSHA256        string
	targetCommand              coreLifecycleCommandRecord
	targetConfigMatches        bool
}

// lifecycleCommandExecutor is the single admission owner for lifecycle state.
// A channel-backed token preserves the mutex-style memory ordering while also
// allowing an RPC context to abandon a command before it is admitted. A
// command whose context loses this race never advances the ordering watermark;
// a later reconciliation barrier can therefore prove FENCED_NOT_ADMITTED.
type lifecycleCommandExecutor struct {
	owner chan struct{}
}

func newLifecycleCommandExecutor() *lifecycleCommandExecutor {
	executor := &lifecycleCommandExecutor{owner: make(chan struct{}, 1)}
	executor.owner <- struct{}{}
	return executor
}

func (e *lifecycleCommandExecutor) acquire(ctx context.Context) error {
	if ctx == nil {
		ctx = context.Background()
	}
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-e.owner:
		// A deadline may become observable at the same instant as the token.
		// Give cancellation priority before the caller can admit a command.
		if err := ctx.Err(); err != nil {
			e.owner <- struct{}{}
			return err
		}
		return nil
	}
}

func (e *lifecycleCommandExecutor) release() {
	e.owner <- struct{}{}
}

// lifecycleStartExecution arbitrates one Start candidate between RPC
// cancellation and publication. Cancellation may stop parsing/box startup,
// but only while publication is still pending. Once tryCommit wins, later
// handler-context cancellation cannot tear down the published runtime; a lost
// response is resolved by the existing sequenced reconciliation barrier.
type lifecycleStartExecution struct {
	ctx             context.Context
	mu              sync.Mutex
	cancelled       bool
	committed       bool
	finished        bool
	candidateCancel context.CancelFunc
	stopWatcher     func() bool
}

func newLifecycleStartExecution(ctx context.Context) *lifecycleStartExecution {
	if ctx == nil {
		ctx = context.Background()
	}
	execution := &lifecycleStartExecution{ctx: ctx}
	execution.stopWatcher = context.AfterFunc(ctx, execution.cancelBeforeCommit)
	return execution
}

func (e *lifecycleStartExecution) cancelBeforeCommit() {
	e.mu.Lock()
	if e.committed || e.finished {
		e.mu.Unlock()
		return
	}
	e.cancelled = true
	cancel := e.candidateCancel
	e.mu.Unlock()
	if cancel != nil {
		cancel()
	}
}

func (e *lifecycleStartExecution) bindCandidateCancel(cancel context.CancelFunc) {
	if cancel == nil {
		return
	}
	e.mu.Lock()
	if e.committed || e.finished {
		e.mu.Unlock()
		return
	}
	e.candidateCancel = cancel
	cancelNow := e.cancelled || e.ctx.Err() != nil
	if cancelNow {
		e.cancelled = true
	}
	e.mu.Unlock()
	if cancelNow {
		cancel()
	}
}

func (e *lifecycleStartExecution) tryCommit() error {
	e.mu.Lock()
	if e.cancelled || e.ctx.Err() != nil {
		e.cancelled = true
		cancel := e.candidateCancel
		e.mu.Unlock()
		if cancel != nil {
			cancel()
		}
		if err := e.ctx.Err(); err != nil {
			return err
		}
		return context.Canceled
	}
	e.committed = true
	e.candidateCancel = nil
	stopWatcher := e.stopWatcher
	e.stopWatcher = nil
	e.mu.Unlock()
	if stopWatcher != nil {
		stopWatcher()
	}
	return nil
}

func (e *lifecycleStartExecution) finish() {
	e.mu.Lock()
	if e.finished {
		e.mu.Unlock()
		return
	}
	e.finished = true
	cancel := e.candidateCancel
	e.candidateCancel = nil
	stopWatcher := e.stopWatcher
	e.stopWatcher = nil
	committed := e.committed
	e.mu.Unlock()
	if stopWatcher != nil {
		stopWatcher()
	}
	if !committed && cancel != nil {
		cancel()
	}
}

// managedCore contains every capability belonging to one sing-box generation.
// It is published only after the candidate has started successfully. Callers
// never retain this object directly; they retain a generation-bound reference
// and revalidate it under coreLifecycle.mu before every use.
type managedCore struct {
	generation           uint64
	startCommandSequence uint64
	configSHA256         string
	cancel               context.CancelFunc
	close                func() error
	dial                 func(context.Context, string, string) (net.Conn, error)
	queryStats           func(context.Context, string, bool) (int64, error)
	cancelled            bool
}

func (c *managedCore) shutdown() error {
	if !c.cancelled {
		c.cancelled = true
		if c.cancel != nil {
			c.cancel()
		}
	}
	if c.close == nil {
		return fmt.Errorf("%w: close capability is missing", errInvalidCoreCandidate)
	}
	return c.close()
}

type coreInstanceReference struct {
	owner      *coreLifecycle
	generation uint64
	wasActive  bool
}

type coreLifecycle struct {
	executor             *lifecycleCommandExecutor
	phase                coreLifecyclePhase
	generation           uint64
	current              *managedCore
	blockedCause         error
	lastCommandSequence  uint64
	lastLifecycleCommand coreLifecycleCommandRecord
}

func newCoreLifecycle() *coreLifecycle {
	return &coreLifecycle{
		executor: newLifecycleCommandExecutor(),
		phase:    coreLifecycleStopped,
	}
}

var activeCoreLifecycle = newCoreLifecycle()

func (l *coreLifecycle) blockedErrorLocked() error {
	if l.blockedCause == nil {
		return errCoreLifecycleBlocked
	}
	return fmt.Errorf("%w: %w", errCoreLifecycleBlocked, l.blockedCause)
}

func (l *coreLifecycle) blockWithCandidateLocked(candidate *managedCore, cause error) error {
	if candidate != nil {
		candidate.generation = l.generation
	}
	l.current = candidate
	l.phase = coreLifecycleBlocked
	l.blockedCause = cause
	return l.blockedErrorLocked()
}

// acceptCommandLocked turns the GUI's monotonically increasing command
// sequence into the server-side ordering authority. gRPC handlers may acquire
// mu in a different order from request issuance; once a newer Stop or Exit is
// accepted, an older delayed Start must never be allowed to run afterwards.
func (l *coreLifecycle) acceptCommandLocked(commandSequence uint64) error {
	if commandSequence == 0 {
		return errInvalidCommandSequence
	}
	if commandSequence <= l.lastCommandSequence {
		return fmt.Errorf(
			"%w: received %d after %d",
			errStaleCommandSequence,
			commandSequence,
			l.lastCommandSequence,
		)
	}
	l.lastCommandSequence = commandSequence
	return nil
}

func (l *coreLifecycle) beginLifecycleCommandLocked(
	commandSequence uint64,
	kind coreLifecycleCommandKind,
	configSHA256 string,
) error {
	if err := l.acceptCommandLocked(commandSequence); err != nil {
		return err
	}
	l.lastLifecycleCommand = coreLifecycleCommandRecord{
		sequence:     commandSequence,
		kind:         kind,
		outcome:      coreLifecycleOutcomeUnspecified,
		configSHA256: configSHA256,
	}
	return nil
}

func (l *coreLifecycle) finishLifecycleCommandLocked(outcome coreLifecycleCommandOutcome) {
	l.lastLifecycleCommand.outcome = outcome
}

// start serializes candidate construction with every lifecycle operation. The
// candidate may own live OS resources while its factory runs, but it is not
// visible through currentReference/dial/queryStats until the factory succeeds
// and the generation is atomically published below.
func (l *coreLifecycle) start(
	commandSequence uint64,
	configSHA256 string,
	factory func() (*managedCore, error),
) error {
	return l.startContext(
		context.Background(),
		commandSequence,
		configSHA256,
		func(*lifecycleStartExecution) (*managedCore, error) { return factory() },
	)
}

func (l *coreLifecycle) startContext(
	ctx context.Context,
	commandSequence uint64,
	configSHA256 string,
	factory func(*lifecycleStartExecution) (*managedCore, error),
) error {
	if err := l.executor.acquire(ctx); err != nil {
		return err
	}
	defer l.executor.release()
	if l.phase == coreLifecycleExiting {
		return errCoreLifecycleExiting
	}
	if err := l.beginLifecycleCommandLocked(
		commandSequence,
		coreLifecycleCommandStart,
		configSHA256,
	); err != nil {
		return err
	}

	switch l.phase {
	case coreLifecycleBlocked:
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeBlocked)
		return l.blockedErrorLocked()
	case coreLifecycleActive:
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeRejectedClean)
		return errCoreAlreadyStarted
	case coreLifecycleStopped:
		// Continue below.
	default:
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeRejectedClean)
		return errCoreLifecycleBusy
	}

	execution := newLifecycleStartExecution(ctx)
	defer execution.finish()
	l.phase = coreLifecycleStarting
	candidate, createErr := factory(execution)
	if createErr == nil && (candidate == nil || candidate.close == nil || candidate.dial == nil) {
		createErr = errInvalidCoreCandidate
	}
	if createErr == nil {
		if commitErr := execution.tryCommit(); commitErr != nil {
			createErr = fmt.Errorf("Start cancelled before runtime publication: %w", commitErr)
		}
	}
	if createErr != nil {
		if candidate == nil {
			l.phase = coreLifecycleStopped
			l.finishLifecycleCommandLocked(coreLifecycleOutcomeFailedClean)
			return createErr
		}
		if closeErr := candidate.shutdown(); closeErr != nil {
			l.generation++
			candidate.startCommandSequence = commandSequence
			candidate.configSHA256 = configSHA256
			l.finishLifecycleCommandLocked(coreLifecycleOutcomeBlocked)
			return l.blockWithCandidateLocked(
				candidate,
				errors.Join(createErr, fmt.Errorf("candidate shutdown failed: %w", closeErr)),
			)
		}
		l.phase = coreLifecycleStopped
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeFailedClean)
		return createErr
	}

	l.generation++
	candidate.generation = l.generation
	candidate.startCommandSequence = commandSequence
	candidate.configSHA256 = configSHA256
	l.current = candidate
	l.blockedCause = nil
	l.phase = coreLifecycleActive
	l.finishLifecycleCommandLocked(coreLifecycleOutcomeSucceeded)
	return nil
}

// stop invalidates all generation references before cancellation and Close.
// A Close error is indeterminate: the resource remains retained but unusable,
// and this process refuses all later starts, dials, stats reads, and stop
// retries. Recovery requires replacing the daemon process.
func (l *coreLifecycle) stop(commandSequence uint64) error {
	return l.stopContext(context.Background(), commandSequence)
}

func (l *coreLifecycle) stopContext(ctx context.Context, commandSequence uint64) error {
	if err := l.executor.acquire(ctx); err != nil {
		return err
	}
	defer l.executor.release()
	if l.phase == coreLifecycleExiting {
		return errCoreLifecycleExiting
	}
	configSHA256 := ""
	if l.current != nil {
		configSHA256 = l.current.configSHA256
	}
	if err := l.beginLifecycleCommandLocked(
		commandSequence,
		coreLifecycleCommandStop,
		configSHA256,
	); err != nil {
		return err
	}

	switch l.phase {
	case coreLifecycleStopped:
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeSucceeded)
		return nil
	case coreLifecycleBlocked:
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeBlocked)
		return l.blockedErrorLocked()
	case coreLifecycleActive:
		// Continue below.
	default:
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeRejectedClean)
		return errCoreLifecycleBusy
	}

	current := l.current
	l.phase = coreLifecycleStopping
	l.generation++ // invalidate every previously issued reference first
	if current == nil {
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeBlocked)
		return l.blockWithCandidateLocked(nil, errInvalidCoreCandidate)
	}
	if closeErr := current.shutdown(); closeErr != nil {
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeBlocked)
		return l.blockWithCandidateLocked(
			current,
			fmt.Errorf("active generation %d shutdown failed: %w", current.generation, closeErr),
		)
	}

	l.current = nil
	l.blockedCause = nil
	l.phase = coreLifecycleStopped
	l.finishLifecycleCommandLocked(coreLifecycleOutcomeSucceeded)
	return nil
}

func (l *coreLifecycle) currentReference() *coreInstanceReference {
	_ = l.executor.acquire(context.Background())
	defer l.executor.release()
	return &coreInstanceReference{
		owner:      l,
		generation: l.generation,
		wasActive:  l.phase == coreLifecycleActive && l.current != nil,
	}
}

func (l *coreLifecycle) validateReferenceLocked(reference *coreInstanceReference) (*managedCore, error) {
	if reference == nil || reference.owner != l {
		return nil, errForeignCoreReference
	}
	if l.phase == coreLifecycleBlocked {
		return nil, l.blockedErrorLocked()
	}
	if l.phase == coreLifecycleExiting {
		return nil, errCoreLifecycleExiting
	}
	if !reference.wasActive {
		if reference.generation != l.generation || l.phase == coreLifecycleActive {
			return nil, errStaleCoreGeneration
		}
		return nil, boxapi.ErrNoActiveInstance
	}
	if l.phase != coreLifecycleActive || l.current == nil ||
		reference.generation != l.generation ||
		reference.generation != l.current.generation {
		return nil, errStaleCoreGeneration
	}
	return l.current, nil
}

func (l *coreLifecycle) validateReference(reference *coreInstanceReference) error {
	_ = l.executor.acquire(context.Background())
	defer l.executor.release()
	_, err := l.validateReferenceLocked(reference)
	return err
}

func (l *coreLifecycle) dial(
	ctx context.Context,
	reference *coreInstanceReference,
	network string,
	address string,
) (net.Conn, error) {
	if err := l.executor.acquire(ctx); err != nil {
		return nil, err
	}
	defer l.executor.release()
	current, err := l.validateReferenceLocked(reference)
	if err != nil {
		return nil, err
	}
	return current.dial(ctx, network, address)
}

func (l *coreLifecycle) queryStats(ctx context.Context, name string, reset bool) (int64, error) {
	if err := l.executor.acquire(ctx); err != nil {
		return 0, err
	}
	defer l.executor.release()
	if l.phase == coreLifecycleBlocked {
		return 0, l.blockedErrorLocked()
	}
	if l.phase == coreLifecycleExiting {
		return 0, errCoreLifecycleExiting
	}
	if l.phase != coreLifecycleActive || l.current == nil {
		return 0, boxapi.ErrNoActiveInstance
	}
	if l.current.queryStats == nil {
		return 0, nil
	}
	return l.current.queryStats(ctx, name, reset)
}

// requestExit atomically changes a precisely stopped lifecycle into a terminal
// exiting fence. The RPC handler may acknowledge this snapshot before asking
// the gRPC owner to shut down. No later lifecycle command is admitted, closing
// the check/exit race without implicitly stopping an active or blocked core.
func (l *coreLifecycle) requestExit(commandSequence uint64) (coreLifecycleSnapshot, error) {
	return l.requestExitContext(context.Background(), commandSequence)
}

func (l *coreLifecycle) requestExitContext(
	ctx context.Context,
	commandSequence uint64,
) (coreLifecycleSnapshot, error) {
	if err := l.executor.acquire(ctx); err != nil {
		return coreLifecycleSnapshot{}, err
	}
	defer l.executor.release()
	if l.phase == coreLifecycleExiting {
		return coreLifecycleSnapshot{}, errCoreLifecycleExiting
	}
	if err := l.beginLifecycleCommandLocked(
		commandSequence,
		coreLifecycleCommandExit,
		"",
	); err != nil {
		return coreLifecycleSnapshot{}, err
	}
	if l.phase == coreLifecycleBlocked {
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeBlocked)
		return coreLifecycleSnapshot{}, l.blockedErrorLocked()
	}
	if l.phase != coreLifecycleStopped || l.current != nil {
		l.finishLifecycleCommandLocked(coreLifecycleOutcomeRejectedClean)
		return coreLifecycleSnapshot{}, errCoreExitRequiresStop
	}
	l.phase = coreLifecycleExiting
	l.finishLifecycleCommandLocked(coreLifecycleOutcomeSucceeded)
	return l.snapshotForCommandLocked(l.lastLifecycleCommand, ""), nil
}

// reconcile is a stateful ordering barrier that returns a runtime snapshot. It
// shares and advances the lifecycle sequence watermark with Start/Stop/Exit. If
// the target command acquired the executor first, reconcile waits for its final
// outcome; if reconcile acquires first, advancing the watermark guarantees
// that a delayed lower-sequence target can no longer mutate this daemon.
func (l *coreLifecycle) reconcile(
	barrierSequence uint64,
	targetSequence uint64,
	targetKind coreLifecycleCommandKind,
	targetConfigSHA256 string,
) (coreLifecycleSnapshot, error) {
	return l.reconcileContext(
		context.Background(),
		barrierSequence,
		targetSequence,
		targetKind,
		targetConfigSHA256,
	)
}

func (l *coreLifecycle) reconcileContext(
	ctx context.Context,
	barrierSequence uint64,
	targetSequence uint64,
	targetKind coreLifecycleCommandKind,
	targetConfigSHA256 string,
) (coreLifecycleSnapshot, error) {
	if err := l.executor.acquire(ctx); err != nil {
		return coreLifecycleSnapshot{}, err
	}
	defer l.executor.release()
	if l.phase == coreLifecycleExiting {
		return coreLifecycleSnapshot{}, errCoreLifecycleExiting
	}
	if targetSequence == 0 || targetSequence >= barrierSequence ||
		targetKind == coreLifecycleCommandUnspecified {
		return coreLifecycleSnapshot{}, errInvalidCommandSequence
	}
	if err := l.acceptCommandLocked(barrierSequence); err != nil {
		return coreLifecycleSnapshot{}, err
	}

	target := coreLifecycleCommandRecord{
		sequence: targetSequence,
		kind:     targetKind,
	}
	switch {
	case l.lastLifecycleCommand.sequence == targetSequence &&
		l.lastLifecycleCommand.kind == targetKind:
		target = l.lastLifecycleCommand
	case l.lastLifecycleCommand.sequence < targetSequence:
		target.outcome = coreLifecycleOutcomeFencedNotAdmitted
	default:
		target.outcome = coreLifecycleOutcomeSupersededUnknown
	}

	return l.snapshotForCommandLocked(target, targetConfigSHA256), nil
}

func (l *coreLifecycle) snapshotForCommandLocked(
	target coreLifecycleCommandRecord,
	targetConfigSHA256 string,
) coreLifecycleSnapshot {
	snapshot := coreLifecycleSnapshot{
		phase:             l.phase,
		runtimeGeneration: l.generation,
		hasCurrent:        l.current != nil,
		orderingWatermark: l.lastCommandSequence,
		targetCommand:     target,
		targetConfigMatches: targetConfigSHA256 != "" &&
			target.configSHA256 == targetConfigSHA256,
	}
	if l.current != nil {
		snapshot.activeStartCommandSequence = l.current.startCommandSequence
		snapshot.currentConfigSHA256 = l.current.configSHA256
	}
	return snapshot
}

func (l *coreLifecycle) snapshot() (coreLifecyclePhase, uint64, bool) {
	_ = l.executor.acquire(context.Background())
	defer l.executor.release()
	return l.phase, l.generation, l.current != nil
}
