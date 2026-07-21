package main

import (
	"context"
	"strings"
	"testing"

	"grpc_server/gen"
)

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
