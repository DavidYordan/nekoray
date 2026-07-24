package grpc_server

import (
	"context"
	"net/http"
	"testing"
)

func TestGetBetweenStrIsTotal(t *testing.T) {
	tests := []struct {
		name  string
		value string
		want  string
	}{
		{name: "present", value: "warp=off\nip=192.0.2.1\nts=1\n", want: "192.0.2.1"},
		{name: "missing marker", value: "x", want: ""},
		{name: "short input", value: "", want: ""},
		{name: "missing terminator", value: "ip=192.0.2.1", want: "192.0.2.1"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := getBetweenStr(test.value, "ip=", "\n"); got != test.want {
				t.Fatalf("got %q, want %q", got, test.want)
			}
		})
	}
}

func TestMeasureDownloadRejectsInvalidURL(t *testing.T) {
	if got := measureDownload(context.Background(), &http.Client{}, "://bad-url"); got != "Error" {
		t.Fatalf("got %q, want Error", got)
	}
}
