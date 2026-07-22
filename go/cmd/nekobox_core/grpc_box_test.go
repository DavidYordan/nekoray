package main

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"grpc_server/auth"
	"grpc_server/gen"
)

func lifecycleCommandContext(sequence string) context.Context {
	return metadata.NewIncomingContext(
		context.Background(),
		metadata.Pairs(lifecycleCommandSequenceHeader, sequence),
	)
}

func authenticatedLifecycleCommandContext(t *testing.T, sequence string, daemonID string) context.Context {
	t.Helper()
	ctx := metadata.NewIncomingContext(
		context.Background(),
		metadata.Pairs(
			lifecycleCommandSequenceHeader, sequence,
			"nekoray_auth", "test-token",
			"nekoray_daemon_instance_id", daemonID,
		),
	)
	authenticated, err := (auth.Authenticator{
		Token:            "test-token",
		DaemonInstanceID: daemonID,
	}).Authenticate(ctx)
	if err != nil {
		t.Fatalf("authenticate test context: %v", err)
	}
	return authenticated
}

func configuredExitServer(t *testing.T) (*server, <-chan struct{}) {
	t.Helper()
	service := &server{}
	shutdownRequested := make(chan struct{})
	if !service.ConfigureGracefulShutdown(func() { close(shutdownRequested) }) {
		t.Fatal("configure graceful shutdown callback")
	}
	return service, shutdownRequested
}

func TestIsolatedTestsRequireExplicitBoundedConfig(t *testing.T) {
	tests := []struct {
		name string
		mode gen.TestMode
		want string
	}{
		{name: "url", mode: gen.TestMode_UrlTest, want: "URL Test requires an explicit bounded test configuration"},
		{name: "full", mode: gen.TestMode_FullTest, want: "Full Test requires an explicit bounded test configuration"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, rpcErr := (&server{}).Test(context.Background(), &gen.TestReq{Mode: test.mode})
			if rpcErr != nil {
				t.Fatalf("unexpected RPC error: %v", rpcErr)
			}
			if response == nil || !strings.Contains(response.Error, test.want) {
				t.Fatalf("expected %q, got %#v", test.want, response)
			}
		})
	}
}

func TestServerExitRefusesActiveAndBlockedLifecycle(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()

	activeCoreLifecycle = newCoreLifecycle()
	service, shutdownRequested := configuredExitServer(t)
	active := &fakeManagedCore{}
	if err := activeCoreLifecycle.start(1, "", func() (*managedCore, error) { return active.candidate(), nil }); err != nil {
		t.Fatalf("start active lifecycle: %v", err)
	}
	if _, err := service.Exit(lifecycleCommandContext("2"), &gen.EmptyReq{}); status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("active Exit returned %v", err)
	}
	phase, _, hasCurrent := activeCoreLifecycle.snapshot()
	if phase != coreLifecycleActive || !hasCurrent || active.cancelCount.Load() != 0 || active.closeCount.Load() != 0 {
		t.Fatalf("active Exit changed lifecycle: phase=%v current=%v cancel=%d close=%d",
			phase, hasCurrent, active.cancelCount.Load(), active.closeCount.Load())
	}
	if err := activeCoreLifecycle.stop(3); err != nil {
		t.Fatalf("cleanup active lifecycle: %v", err)
	}

	activeCoreLifecycle = newCoreLifecycle()
	blocked := &fakeManagedCore{closeErr: errFakeClose}
	if err := activeCoreLifecycle.start(1, "", func() (*managedCore, error) { return blocked.candidate(), nil }); err != nil {
		t.Fatalf("start blocked lifecycle candidate: %v", err)
	}
	if err := activeCoreLifecycle.stop(2); !errors.Is(err, errCoreLifecycleBlocked) {
		t.Fatalf("create blocked lifecycle: %v", err)
	}
	if _, err := service.Exit(lifecycleCommandContext("3"), &gen.EmptyReq{}); status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("blocked Exit returned %v", err)
	}
	if blocked.closeCount.Load() != 1 {
		t.Fatalf("blocked Exit retried Close %d time(s)", blocked.closeCount.Load())
	}
	select {
	case <-shutdownRequested:
		t.Fatal("rejected Exit requested graceful shutdown")
	default:
	}
}

func TestServerExitRequiresLifecycleCommandSequence(t *testing.T) {
	if _, err := (&server{}).Exit(context.Background(), &gen.EmptyReq{}); status.Code(err) != codes.InvalidArgument {
		t.Fatalf("missing lifecycle command sequence returned %v", err)
	}
	if _, err := (&server{}).Exit(lifecycleCommandContext("0"), &gen.EmptyReq{}); status.Code(err) != codes.InvalidArgument {
		t.Fatalf("zero lifecycle command sequence returned %v", err)
	}
}

func TestServerExitReturnsExactAckAndRequestsShutdown(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()
	activeCoreLifecycle = newCoreLifecycle()
	service, shutdownRequested := configuredExitServer(t)

	response, err := service.Exit(
		authenticatedLifecycleCommandContext(t, "7", "daemon-test"),
		&gen.EmptyReq{},
	)
	if err != nil {
		t.Fatalf("Exit: %v", err)
	}
	if response.DaemonInstanceId != "daemon-test" ||
		response.OrderingWatermark != 7 ||
		response.Phase != gen.LifecyclePhase_LIFECYCLE_PHASE_EXITING ||
		response.RuntimeGeneration != 0 || response.HasCurrent ||
		response.ActiveStartCommandSequence != 0 ||
		response.CurrentConfigSha256 != "" || response.TargetCommand == nil ||
		response.TargetCommand.Sequence != 7 ||
		response.TargetCommand.Kind != gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT ||
		response.TargetCommand.Outcome != gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_SUCCEEDED ||
		response.TargetCommand.ConfigSha256 != "" ||
		response.TargetCommand.RequestedConfigMatches {
		t.Fatalf("unexpected Exit ACK: %#v", response)
	}
	select {
	case <-shutdownRequested:
	case <-time.After(time.Second):
		t.Fatal("Exit did not request graceful shutdown")
	}
	if _, err := service.QueryStats(context.Background(), &gen.QueryStatsReq{}); status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("QueryStats after Exit returned %v", err)
	}
	if _, err := service.ReconcileLifecycle(
		authenticatedLifecycleCommandContext(t, "8", "daemon-test"),
		&gen.LifecycleReconcileReq{
			TargetCommandSequence: 7,
			TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT,
		},
	); status.Code(err) != codes.FailedPrecondition || !strings.Contains(err.Error(), errCoreLifecycleExiting.Error()) {
		t.Fatalf("Reconcile after committed Exit returned %v", err)
	}
	if _, err := service.Exit(
		authenticatedLifecycleCommandContext(t, "9", "daemon-test"),
		&gen.EmptyReq{},
	); status.Code(err) != codes.FailedPrecondition || !strings.Contains(err.Error(), errCoreLifecycleExiting.Error()) {
		t.Fatalf("second Exit returned %v", err)
	}
}

func TestServerLifecycleCommandSequenceOrdersStopBeforeDelayedStart(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()
	activeCoreLifecycle = newCoreLifecycle()

	stopResponse, err := (&server{}).Stop(lifecycleCommandContext("2"), &gen.EmptyReq{})
	if err != nil || stopResponse == nil || stopResponse.Error != "" {
		t.Fatalf("newer stopped-state Stop failed: response=%#v err=%v", stopResponse, err)
	}
	startResponse, err := (&server{}).Start(lifecycleCommandContext("1"), nil)
	if err != nil || startResponse == nil || !strings.Contains(startResponse.Error, errStaleCommandSequence.Error()) {
		t.Fatalf("delayed older Start was not rejected as stale: response=%#v err=%v", startResponse, err)
	}
}

func TestServerReconcileLifecycleReturnsIdentityAndExactTarget(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()
	activeCoreLifecycle = newCoreLifecycle()

	const configHash = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
	if err := activeCoreLifecycle.start(1, configHash, func() (*managedCore, error) {
		return (&fakeManagedCore{}).candidate(), nil
	}); err != nil {
		t.Fatalf("start lifecycle: %v", err)
	}
	response, err := (&server{}).ReconcileLifecycle(
		authenticatedLifecycleCommandContext(t, "2", "daemon-test"),
		&gen.LifecycleReconcileReq{
			TargetCommandSequence: 1,
			TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_START,
			TargetConfigSha256:    configHash,
		},
	)
	if err != nil {
		t.Fatalf("reconcile RPC: %v", err)
	}
	if response.DaemonInstanceId != "daemon-test" ||
		response.OrderingWatermark != 2 ||
		response.Phase != gen.LifecyclePhase_LIFECYCLE_PHASE_ACTIVE ||
		!response.HasCurrent || response.ActiveStartCommandSequence != 1 ||
		response.CurrentConfigSha256 != configHash || response.TargetCommand == nil ||
		response.TargetCommand.Outcome != gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_SUCCEEDED ||
		!response.TargetCommand.RequestedConfigMatches {
		t.Fatalf("unexpected reconcile response: %#v", response)
	}
}

func TestServerReconcileExitFencesLostRequestWhileStopped(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()
	activeCoreLifecycle = newCoreLifecycle()

	response, err := (&server{}).ReconcileLifecycle(
		authenticatedLifecycleCommandContext(t, "2", "daemon-test"),
		&gen.LifecycleReconcileReq{
			TargetCommandSequence: 1,
			TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT,
			TargetConfigSha256:    "",
		},
	)
	if err != nil {
		t.Fatalf("reconcile lost Exit RPC: %v", err)
	}
	if response.DaemonInstanceId != "daemon-test" ||
		response.OrderingWatermark != 2 ||
		response.Phase != gen.LifecyclePhase_LIFECYCLE_PHASE_STOPPED ||
		response.RuntimeGeneration != 0 || response.HasCurrent ||
		response.ActiveStartCommandSequence != 0 ||
		response.CurrentConfigSha256 != "" || response.TargetCommand == nil ||
		response.TargetCommand.Sequence != 1 ||
		response.TargetCommand.Kind != gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT ||
		response.TargetCommand.Outcome != gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED ||
		response.TargetCommand.ConfigSha256 != "" ||
		response.TargetCommand.RequestedConfigMatches {
		t.Fatalf("unexpected lost Exit reconciliation: %#v", response)
	}

	response, err = (&server{}).ReconcileLifecycle(
		authenticatedLifecycleCommandContext(t, "3", "daemon-test"),
		&gen.LifecycleReconcileReq{
			TargetCommandSequence: 1,
			TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT,
		},
	)
	if err != nil || response.OrderingWatermark != 3 ||
		response.Phase != gen.LifecyclePhase_LIFECYCLE_PHASE_STOPPED ||
		response.TargetCommand == nil ||
		response.TargetCommand.Outcome != gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED {
		t.Fatalf("retry lost Exit reconciliation: response=%#v err=%v", response, err)
	}

	service, shutdownRequested := configuredExitServer(t)
	if _, err := service.Exit(lifecycleCommandContext("1"), &gen.EmptyReq{}); status.Code(err) != codes.FailedPrecondition ||
		!strings.Contains(err.Error(), errStaleCommandSequence.Error()) {
		t.Fatalf("delayed Exit was not fenced: %v", err)
	}
	select {
	case <-shutdownRequested:
		t.Fatal("fenced delayed Exit requested graceful shutdown")
	default:
	}
}

func TestServerReconcileExitHashContract(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()
	activeCoreLifecycle = newCoreLifecycle()

	_, err := (&server{}).ReconcileLifecycle(
		authenticatedLifecycleCommandContext(t, "2", "daemon-test"),
		&gen.LifecycleReconcileReq{
			TargetCommandSequence: 1,
			TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT,
			TargetConfigSha256:    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		},
	)
	if status.Code(err) != codes.InvalidArgument {
		t.Fatalf("Exit reconciliation with config hash returned %v", err)
	}

	// Invalid input must not consume the barrier sequence.
	response, err := (&server{}).ReconcileLifecycle(
		authenticatedLifecycleCommandContext(t, "2", "daemon-test"),
		&gen.LifecycleReconcileReq{
			TargetCommandSequence: 1,
			TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT,
		},
	)
	if err != nil || response.OrderingWatermark != 2 {
		t.Fatalf("valid empty-hash Exit reconciliation after invalid request: response=%#v err=%v", response, err)
	}
}

func TestServerReconcileRequiresHigherBarrierSequence(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()
	activeCoreLifecycle = newCoreLifecycle()

	_, err := (&server{}).ReconcileLifecycle(
		authenticatedLifecycleCommandContext(t, "1", "daemon-test"),
		&gen.LifecycleReconcileReq{
			TargetCommandSequence: 1,
			TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_START,
			TargetConfigSha256:    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
		},
	)
	if status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("non-higher reconcile barrier returned %v", err)
	}
}

func TestServerReconcileRejectsMalformedHashBeforeAdvancingWatermark(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()

	for _, malformedHash := range []string{
		"short",
		"EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE",
		"gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg",
	} {
		activeCoreLifecycle = newCoreLifecycle()
		_, err := (&server{}).ReconcileLifecycle(
			authenticatedLifecycleCommandContext(t, "2", "daemon-test"),
			&gen.LifecycleReconcileReq{
				TargetCommandSequence: 1,
				TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_START,
				TargetConfigSha256:    malformedHash,
			},
		)
		if status.Code(err) != codes.InvalidArgument {
			t.Fatalf("malformed hash %q returned %v", malformedHash, err)
		}

		const validHash = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
		if err := activeCoreLifecycle.start(1, validHash, func() (*managedCore, error) {
			return (&fakeManagedCore{}).candidate(), nil
		}); err != nil {
			t.Fatalf("malformed reconcile advanced lifecycle watermark: %v", err)
		}
		if err := activeCoreLifecycle.stop(3); err != nil {
			t.Fatalf("cleanup lifecycle: %v", err)
		}
	}
}
