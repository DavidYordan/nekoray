package auth

import (
	"context"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

func incoming(headers ...string) context.Context {
	return metadata.NewIncomingContext(context.Background(), metadata.Pairs(headers...))
}

func TestAuthenticateBindsTokenAndDaemonIdentity(t *testing.T) {
	authenticator := Authenticator{
		Token:            "session-token",
		DaemonInstanceID: "daemon-a",
	}
	ctx, err := authenticator.Authenticate(incoming(
		"nekoray_auth", "session-token",
		"nekoray_daemon_instance_id", "daemon-a",
		"unrelated", "kept",
	))
	if err != nil {
		t.Fatalf("authenticate: %v", err)
	}
	if DaemonInstanceID(ctx) != "daemon-a" {
		t.Fatalf("verified daemon identity was not retained in context")
	}
	md, ok := metadata.FromIncomingContext(ctx)
	if !ok || len(md.Get("nekoray_auth")) != 0 ||
		len(md.Get("nekoray_daemon_instance_id")) != 0 ||
		len(md.Get("unrelated")) != 1 {
		t.Fatalf("unexpected authenticated metadata: %#v", md)
	}
}

func TestAuthenticateRejectsWrongDaemonBeforeHandler(t *testing.T) {
	authenticator := Authenticator{
		Token:            "session-token",
		DaemonInstanceID: "daemon-new",
	}
	for _, test := range []struct {
		name    string
		headers []string
	}{
		{
			name: "old identity",
			headers: []string{
				"nekoray_auth", "session-token",
				"nekoray_daemon_instance_id", "daemon-old",
			},
		},
		{
			name: "missing identity",
			headers: []string{
				"nekoray_auth", "session-token",
			},
		},
		{
			name: "duplicate identity",
			headers: []string{
				"nekoray_auth", "session-token",
				"nekoray_daemon_instance_id", "daemon-new",
				"nekoray_daemon_instance_id", "daemon-new",
			},
		},
	} {
		t.Run(test.name, func(t *testing.T) {
			if _, err := authenticator.Authenticate(incoming(test.headers...)); status.Code(err) != codes.Unauthenticated {
				t.Fatalf("expected Unauthenticated, got %v", err)
			}
		})
	}
}
