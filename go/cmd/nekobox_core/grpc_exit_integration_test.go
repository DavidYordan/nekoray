package main

import (
	"context"
	"net"
	"sync/atomic"
	"testing"
	"time"

	grpc_auth "github.com/grpc-ecosystem/go-grpc-middleware/auth"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"grpc_server/auth"
	"grpc_server/gen"
)

func TestExitAckIsDeliveredBeforeGracefulServerStop(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()
	activeCoreLifecycle = newCoreLifecycle()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	const (
		token    = "exit-integration-token"
		daemonID = "exit-integration-daemon"
	)
	authed := auth.Authenticator{Token: token, DaemonInstanceID: daemonID}
	grpcServer := grpc.NewServer(
		grpc.UnaryInterceptor(grpc_auth.UnaryServerInterceptor(authed.Authenticate)),
	)
	service := &server{}
	var shutdownCalls atomic.Int32
	if !service.ConfigureGracefulShutdown(func() {
		shutdownCalls.Add(1)
		grpcServer.GracefulStop()
	}) {
		t.Fatal("configure graceful shutdown callback")
	}
	gen.RegisterLibcoreServiceServer(grpcServer, service)
	serveDone := make(chan error, 1)
	go func() { serveDone <- grpcServer.Serve(listener) }()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	connection, err := grpc.DialContext(
		ctx,
		listener.Addr().String(),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock(),
	)
	if err != nil {
		grpcServer.Stop()
		t.Fatalf("dial: %v", err)
	}
	defer connection.Close()

	exitContext := metadata.NewOutgoingContext(
		ctx,
		metadata.Pairs(
			"nekoray_auth", token,
			"nekoray_daemon_instance_id", daemonID,
			lifecycleCommandSequenceHeader, "11",
		),
	)
	response, err := gen.NewLibcoreServiceClient(connection).Exit(
		exitContext,
		&gen.EmptyReq{},
	)
	if err != nil {
		grpcServer.Stop()
		t.Fatalf("Exit RPC did not deliver its ACK: %v", err)
	}
	if response.DaemonInstanceId != daemonID ||
		response.OrderingWatermark != 11 ||
		response.Phase != gen.LifecyclePhase_LIFECYCLE_PHASE_EXITING ||
		response.HasCurrent || response.ActiveStartCommandSequence != 0 ||
		response.CurrentConfigSha256 != "" || response.TargetCommand == nil ||
		response.TargetCommand.Sequence != 11 ||
		response.TargetCommand.Kind != gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT ||
		response.TargetCommand.Outcome != gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_SUCCEEDED ||
		response.TargetCommand.ConfigSha256 != "" ||
		response.TargetCommand.RequestedConfigMatches {
		grpcServer.Stop()
		t.Fatalf("unexpected Exit ACK: %#v", response)
	}

	select {
	case serveErr := <-serveDone:
		if serveErr != nil {
			t.Fatalf("Serve after graceful Exit: %v", serveErr)
		}
	case <-ctx.Done():
		grpcServer.Stop()
		t.Fatalf("server did not stop gracefully after ACK: %v", ctx.Err())
	}
	if shutdownCalls.Load() != 1 {
		t.Fatalf("graceful shutdown callback ran %d times", shutdownCalls.Load())
	}
}
