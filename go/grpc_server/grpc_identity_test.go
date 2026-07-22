package grpc_server

import (
	"context"
	"testing"

	"grpc_server/auth"
	"grpc_server/gen"

	"google.golang.org/grpc/metadata"
)

func TestGetDaemonInfoEchoesAuthenticatedIdentity(t *testing.T) {
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
