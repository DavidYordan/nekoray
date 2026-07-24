package main

import (
	"context"
	"net"
	"sync/atomic"
	"testing"
	"time"

	grpc_auth "github.com/grpc-ecosystem/go-grpc-middleware/auth"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
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

func TestGrpcDeadlineCancelsQueuedStopBeforeAdmission(t *testing.T) {
	originalLifecycle := activeCoreLifecycle
	defer func() { activeCoreLifecycle = originalLifecycle }()
	activeCoreLifecycle = newCoreLifecycle()
	const (
		token      = "deadline-integration-token"
		daemonID   = "deadline-integration-daemon"
		configHash = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
	)
	fake := &fakeManagedCore{
		queryValue: 1,
		queryEnter: make(chan struct{}),
		queryWait:  make(chan struct{}),
	}
	if err := activeCoreLifecycle.start(1, configHash, func() (*managedCore, error) {
		return fake.candidate(), nil
	}); err != nil {
		t.Fatalf("start active lifecycle: %v", err)
	}
	queryDone := make(chan error, 1)
	go func() {
		_, err := activeCoreLifecycle.queryStats(context.Background(), "proxy", false)
		queryDone <- err
	}()
	<-fake.queryEnter

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		close(fake.queryWait)
		<-queryDone
		t.Fatalf("listen: %v", err)
	}
	authed := auth.Authenticator{Token: token, DaemonInstanceID: daemonID}
	serverStopContextDone := make(chan struct{})
	grpcServer := grpc.NewServer(
		grpc.ChainUnaryInterceptor(
			grpc_auth.UnaryServerInterceptor(authed.Authenticate),
			func(
				ctx context.Context,
				req interface{},
				info *grpc.UnaryServerInfo,
				handler grpc.UnaryHandler,
			) (interface{}, error) {
				if info.FullMethod == "/libcore.LibcoreService/Stop" {
					go func() {
						<-ctx.Done()
						close(serverStopContextDone)
					}()
				}
				return handler(ctx, req)
			},
		),
	)
	gen.RegisterLibcoreServiceServer(grpcServer, &server{})
	serveDone := make(chan error, 1)
	go func() { serveDone <- grpcServer.Serve(listener) }()
	defer func() {
		grpcServer.Stop()
		<-serveDone
	}()

	dialContext, cancelDial := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancelDial()
	connection, err := grpc.DialContext(
		dialContext,
		listener.Addr().String(),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock(),
	)
	if err != nil {
		close(fake.queryWait)
		<-queryDone
		t.Fatalf("dial: %v", err)
	}
	defer connection.Close()
	client := gen.NewLibcoreServiceClient(connection)

	stopContext, cancelStop := context.WithTimeout(context.Background(), 75*time.Millisecond)
	stopContext = metadata.NewOutgoingContext(
		stopContext,
		metadata.Pairs(
			"nekoray_auth", token,
			"nekoray_daemon_instance_id", daemonID,
			lifecycleCommandSequenceHeader, "2",
		),
	)
	response, stopErr := client.Stop(stopContext, &gen.EmptyReq{})
	cancelStop()
	if response != nil || status.Code(stopErr) != codes.DeadlineExceeded {
		close(fake.queryWait)
		<-queryDone
		t.Fatalf("queued Stop returned response=%#v error=%v", response, stopErr)
	}
	// A client-side deadline may be observed before HTTP/2 cancellation reaches
	// the server handler. Keep the admission gate occupied until the server
	// context itself is done, then prove that the waiting command is fenced.
	select {
	case <-serverStopContextDone:
	case <-time.After(time.Second):
		close(fake.queryWait)
		<-queryDone
		t.Fatal("Stop deadline did not reach the server context")
	}
	close(fake.queryWait)
	if err := <-queryDone; err != nil {
		t.Fatalf("release query: %v", err)
	}

	reconcileContext, cancelReconcile := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancelReconcile()
	reconcileContext = metadata.NewOutgoingContext(
		reconcileContext,
		metadata.Pairs(
			"nekoray_auth", token,
			"nekoray_daemon_instance_id", daemonID,
			lifecycleCommandSequenceHeader, "3",
		),
	)
	reconciled, err := client.ReconcileLifecycle(
		reconcileContext,
		&gen.LifecycleReconcileReq{
			TargetCommandSequence: 2,
			TargetCommandKind:     gen.LifecycleCommandKind_LIFECYCLE_COMMAND_STOP,
			TargetConfigSha256:    configHash,
		},
	)
	if err != nil {
		t.Fatalf("reconcile expired Stop: %v", err)
	}
	if reconciled.Phase != gen.LifecyclePhase_LIFECYCLE_PHASE_ACTIVE ||
		!reconciled.HasCurrent || reconciled.CurrentConfigSha256 != configHash ||
		reconciled.TargetCommand == nil ||
		reconciled.TargetCommand.Sequence != 2 ||
		reconciled.TargetCommand.Kind != gen.LifecycleCommandKind_LIFECYCLE_COMMAND_STOP ||
		reconciled.TargetCommand.Outcome !=
			gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED ||
		fake.cancelCount.Load() != 0 || fake.closeCount.Load() != 0 {
		t.Fatalf("expired queued Stop mutated runtime: response=%#v cancel=%d close=%d",
			reconciled, fake.cancelCount.Load(), fake.closeCount.Load())
	}
}
