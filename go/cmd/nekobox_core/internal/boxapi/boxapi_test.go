package boxapi

import (
	"context"
	"errors"
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
