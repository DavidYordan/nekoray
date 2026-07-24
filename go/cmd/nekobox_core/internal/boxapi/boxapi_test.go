package boxapi

import (
	"context"
	"errors"
	"net"
	"net/http"
	"testing"
)

func TestDialContextRejectsNilInstance(t *testing.T) {
	conn, err := DialContext(context.Background(), nil, nil, "tcp", "127.0.0.1:1")
	if conn != nil {
		conn.Close()
		t.Fatal("nil instance unexpectedly returned a connection")
	}
	if !errors.Is(err, ErrNoActiveInstance) {
		t.Fatalf("expected ErrNoActiveInstance, got %v", err)
	}
}

func TestHTTPClientRejectsNilInstanceWithoutSystemFallback(t *testing.T) {
	client := CreateProxyHttpClient(nil, nil)
	response, err := client.Get("http://127.0.0.1:1/")
	if response != nil {
		response.Body.Close()
		t.Fatal("nil instance unexpectedly completed an HTTP request")
	}
	if !errors.Is(err, ErrNoActiveInstance) {
		t.Fatalf("expected ErrNoActiveInstance, got %v", err)
	}
}

func TestGenerationBoundHTTPClientDisablesConnectionReuse(t *testing.T) {
	client := CreateGenerationBoundHTTPClient(func(context.Context, string, string) (net.Conn, error) {
		return nil, ErrNoActiveInstance
	})
	transport, ok := client.Transport.(*http.Transport)
	if !ok {
		t.Fatalf("unexpected transport type %T", client.Transport)
	}
	if !transport.DisableKeepAlives {
		t.Fatal("generation-bound HTTP client may reuse a connection after generation invalidation")
	}
}
