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
	mu                   sync.Mutex
	phase                coreLifecyclePhase
	generation           uint64
	current              *managedCore
	blockedCause         error
	lastCommandSequence  uint64
	lastLifecycleCommand coreLifecycleCommandRecord
}

func newCoreLifecycle() *coreLifecycle {
	return &coreLifecycle{phase: coreLifecycleStopped}
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
	l.mu.Lock()
	defer l.mu.Unlock()
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

	l.phase = coreLifecycleStarting
	candidate, createErr := factory()
	if createErr == nil && (candidate == nil || candidate.close == nil || candidate.dial == nil) {
		createErr = errInvalidCoreCandidate
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
	l.mu.Lock()
	defer l.mu.Unlock()
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
	l.mu.Lock()
	defer l.mu.Unlock()
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
	l.mu.Lock()
	defer l.mu.Unlock()
	_, err := l.validateReferenceLocked(reference)
	return err
}

func (l *coreLifecycle) dial(
	ctx context.Context,
	reference *coreInstanceReference,
	network string,
	address string,
) (net.Conn, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	current, err := l.validateReferenceLocked(reference)
	if err != nil {
		return nil, err
	}
	return current.dial(ctx, network, address)
}

func (l *coreLifecycle) queryStats(ctx context.Context, name string, reset bool) (int64, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
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
	l.mu.Lock()
	defer l.mu.Unlock()
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
// the target command acquired the mutex first, reconcile waits for its final
// outcome; if reconcile acquires first, advancing the watermark guarantees
// that a delayed lower-sequence target can no longer mutate this daemon.
func (l *coreLifecycle) reconcile(
	barrierSequence uint64,
	targetSequence uint64,
	targetKind coreLifecycleCommandKind,
	targetConfigSHA256 string,
) (coreLifecycleSnapshot, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
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
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.phase, l.generation, l.current != nil
}
