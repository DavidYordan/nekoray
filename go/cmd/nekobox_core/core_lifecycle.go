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
	errInvalidCoreCandidate   = errors.New("invalid core lifecycle candidate")
	errStaleCoreGeneration    = errors.New("stale core generation reference")
	errForeignCoreReference   = errors.New("core generation reference belongs to another lifecycle owner")
	errCoreExitRequiresStop   = errors.New("core lifecycle must be stopped before daemon exit")
	errInvalidCommandSequence = errors.New("core lifecycle command sequence must be non-zero")
	errStaleCommandSequence   = errors.New("stale core lifecycle command sequence")
	errProcessExitReturned    = errors.New("process exit callback returned unexpectedly")
)

type coreLifecyclePhase uint8

const (
	coreLifecycleStopped coreLifecyclePhase = iota
	coreLifecycleStarting
	coreLifecycleActive
	coreLifecycleStopping
	coreLifecycleBlocked
)

// managedCore contains every capability belonging to one sing-box generation.
// It is published only after the candidate has started successfully. Callers
// never retain this object directly; they retain a generation-bound reference
// and revalidate it under coreLifecycle.mu before every use.
type managedCore struct {
	generation uint64
	cancel     context.CancelFunc
	close      func() error
	dial       func(context.Context, string, string) (net.Conn, error)
	queryStats func(context.Context, string, bool) (int64, error)
	cancelled  bool
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
	mu                  sync.Mutex
	phase               coreLifecyclePhase
	generation          uint64
	current             *managedCore
	blockedCause        error
	lastCommandSequence uint64
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

// start serializes candidate construction with every lifecycle operation. The
// candidate may own live OS resources while its factory runs, but it is not
// visible through currentReference/dial/queryStats until the factory succeeds
// and the generation is atomically published below.
func (l *coreLifecycle) start(
	commandSequence uint64,
	factory func() (*managedCore, error),
) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if err := l.acceptCommandLocked(commandSequence); err != nil {
		return err
	}

	switch l.phase {
	case coreLifecycleBlocked:
		return l.blockedErrorLocked()
	case coreLifecycleActive:
		return errCoreAlreadyStarted
	case coreLifecycleStopped:
		// Continue below.
	default:
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
			return createErr
		}
		if closeErr := candidate.shutdown(); closeErr != nil {
			l.generation++
			return l.blockWithCandidateLocked(
				candidate,
				errors.Join(createErr, fmt.Errorf("candidate shutdown failed: %w", closeErr)),
			)
		}
		l.phase = coreLifecycleStopped
		return createErr
	}

	l.generation++
	candidate.generation = l.generation
	l.current = candidate
	l.blockedCause = nil
	l.phase = coreLifecycleActive
	return nil
}

// stop invalidates all generation references before cancellation and Close.
// A Close error is indeterminate: the resource remains retained but unusable,
// and this process refuses all later starts, dials, stats reads, and stop
// retries. Recovery requires replacing the daemon process.
func (l *coreLifecycle) stop(commandSequence uint64) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if err := l.acceptCommandLocked(commandSequence); err != nil {
		return err
	}

	switch l.phase {
	case coreLifecycleStopped:
		return nil
	case coreLifecycleBlocked:
		return l.blockedErrorLocked()
	case coreLifecycleActive:
		// Continue below.
	default:
		return errCoreLifecycleBusy
	}

	current := l.current
	l.phase = coreLifecycleStopping
	l.generation++ // invalidate every previously issued reference first
	if current == nil {
		return l.blockWithCandidateLocked(nil, errInvalidCoreCandidate)
	}
	if closeErr := current.shutdown(); closeErr != nil {
		return l.blockWithCandidateLocked(
			current,
			fmt.Errorf("active generation %d shutdown failed: %w", current.generation, closeErr),
		)
	}

	l.current = nil
	l.blockedCause = nil
	l.phase = coreLifecycleStopped
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
	if l.phase != coreLifecycleActive || l.current == nil {
		return 0, boxapi.ErrNoActiveInstance
	}
	if l.current.queryStats == nil {
		return 0, nil
	}
	return l.current.queryStats(ctx, name, reset)
}

// exitWhenStopped keeps the lifecycle mutex until the process terminates. This
// closes the check/exit race: no Start can publish a generation after Exit has
// verified the stopped state. Exit never performs an implicit Stop because it
// cannot know whether doing so would remove TUN or another fail-closed guard.
func (l *coreLifecycle) exitWhenStopped(
	commandSequence uint64,
	exitProcess func(int),
) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if err := l.acceptCommandLocked(commandSequence); err != nil {
		return err
	}
	if l.phase == coreLifecycleBlocked {
		return l.blockedErrorLocked()
	}
	if l.phase != coreLifecycleStopped || l.current != nil {
		return errCoreExitRequiresStop
	}
	exitProcess(0)
	return errProcessExitReturned
}

func (l *coreLifecycle) snapshot() (coreLifecyclePhase, uint64, bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.phase, l.generation, l.current != nil
}
