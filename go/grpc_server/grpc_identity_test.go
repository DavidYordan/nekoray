package grpc_server

import (
	"context"
	"sync/atomic"
	"testing"
	"time"

	"grpc_server/auth"
	"grpc_server/gen"

	"google.golang.org/grpc/metadata"
)

func TestGetDaemonInfoEchoesAuthenticatedIdentity(t *testing.T) {
	if LifecycleProtocolVersion != 3 {
		t.Fatalf("unexpected lifecycle protocol version %d", LifecycleProtocolVersion)
	}
	ctx := metadata.NewIncomingContext(
		context.Background(),
		metadata.Pairs(
			"nekoray_auth", "test-token",
			"nekoray_daemon_instance_id", "daemon-test",
		),
	)
	authenticated, err := (auth.Authenticator{
		Token:            "test-token",
		DaemonInstanceID: "daemon-test",
	}).Authenticate(ctx)
	if err != nil {
		t.Fatalf("authenticate: %v", err)
	}
	response, err := (&BaseServer{}).GetDaemonInfo(authenticated, &gen.EmptyReq{})
	if err != nil {
		t.Fatalf("GetDaemonInfo: %v", err)
	}
	if response.DaemonInstanceId != "daemon-test" ||
		response.LifecycleProtocolVersion != LifecycleProtocolVersion {
		t.Fatalf("unexpected daemon info: %#v", response)
	}
}

func TestGracefulShutdownControllerIsConfiguredAndRequestedOnce(t *testing.T) {
	server := &BaseServer{}
	var calls atomic.Int32
	called := make(chan struct{}, 2)
	if !server.ConfigureGracefulShutdown(func() {
		calls.Add(1)
		called <- struct{}{}
	}) {
		t.Fatal("first graceful shutdown configuration was rejected")
	}
	if !server.GracefulShutdownConfigured() {
		t.Fatal("configured graceful shutdown callback was not observable")
	}
	if server.ConfigureGracefulShutdown(func() {}) {
		t.Fatal("second graceful shutdown configuration was accepted")
	}
	if !server.RequestGracefulShutdown() || !server.RequestGracefulShutdown() {
		t.Fatal("configured graceful shutdown request was rejected")
	}
	select {
	case <-called:
	case <-time.After(time.Second):
		t.Fatal("graceful shutdown callback was not invoked")
	}
	time.Sleep(25 * time.Millisecond)
	if calls.Load() != 1 {
		t.Fatalf("graceful shutdown callback ran %d times", calls.Load())
	}
}
