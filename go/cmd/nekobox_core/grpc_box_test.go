package main

import (
	"context"
	"errors"
	"strings"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"grpc_server/gen"
)

func lifecycleCommandContext(sequence string) context.Context {
	return metadata.NewIncomingContext(
		context.Background(),
		metadata.Pairs(lifecycleCommandSequenceHeader, sequence),
	)
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
	active := &fakeManagedCore{}
	if err := activeCoreLifecycle.start(1, func() (*managedCore, error) { return active.candidate(), nil }); err != nil {
		t.Fatalf("start active lifecycle: %v", err)
	}
	if _, err := (&server{}).Exit(lifecycleCommandContext("2"), &gen.EmptyReq{}); status.Code(err) != codes.FailedPrecondition {
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
	if err := activeCoreLifecycle.start(1, func() (*managedCore, error) { return blocked.candidate(), nil }); err != nil {
		t.Fatalf("start blocked lifecycle candidate: %v", err)
	}
	if err := activeCoreLifecycle.stop(2); !errors.Is(err, errCoreLifecycleBlocked) {
		t.Fatalf("create blocked lifecycle: %v", err)
	}
	if _, err := (&server{}).Exit(lifecycleCommandContext("3"), &gen.EmptyReq{}); status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("blocked Exit returned %v", err)
	}
	if blocked.closeCount.Load() != 1 {
		t.Fatalf("blocked Exit retried Close %d time(s)", blocked.closeCount.Load())
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
