package main

import (
	"context"
	"errors"
	"net"
	"net/http"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/matsuridayo/libneko/neko_log"
	box "github.com/sagernet/sing-box"

	"nekobox_core/internal/boxapi"
)

var (
	errInvalidCoreInstance       = errors.New("invalid sing-box instance; implicit fallback is disabled")
	errDirectUDPFallbackDisabled = errors.New("direct UDP fallback is disabled; use a routed sing-box connection")
)

type failClosedRoundTripper struct {
	err error
}

func (t failClosedRoundTripper) RoundTrip(*http.Request) (*http.Response, error) {
	return nil, t.err
}

type generationGuardRoundTripper struct {
	lifecycle *coreLifecycle
	reference *coreInstanceReference
	next      http.RoundTripper
}

func (t generationGuardRoundTripper) RoundTrip(request *http.Request) (*http.Response, error) {
	if err := t.lifecycle.validateReference(t.reference); err != nil {
		return nil, err
	}
	return t.next.RoundTrip(request)
}

func rejectedHTTPClient(err error) *http.Client {
	return &http.Client{Transport: failClosedRoundTripper{err: err}}
}

func generationBoundHTTPClient(reference *coreInstanceReference) *http.Client {
	if reference == nil || reference.owner == nil {
		return rejectedHTTPClient(errForeignCoreReference)
	}
	client := boxapi.CreateGenerationBoundHTTPClient(
		func(ctx context.Context, network, address string) (net.Conn, error) {
			return reference.owner.dial(ctx, reference, network, address)
		},
	)
	client.Transport = generationGuardRoundTripper{
		lifecycle: reference.owner,
		reference: reference,
		next:      client.Transport,
	}
	return client
}

func setupCore() {
	neko_log.SetupLog(50*1024, "./neko.log")
	//
	neko_common.GetCurrentInstance = func() interface{} {
		// Always return a reference, including while stopped/blocked. Returning
		// nil here would let a caller accidentally bind to a newer generation
		// when it creates its HTTP client later.
		return activeCoreLifecycle.currentReference()
	}
	neko_common.DialContext = func(ctx context.Context, specifiedInstance interface{}, network, addr string) (net.Conn, error) {
		switch value := specifiedInstance.(type) {
		case *coreInstanceReference:
			if value == nil || value.owner == nil {
				return nil, errInvalidCoreInstance
			}
			return value.owner.dial(ctx, value, network, addr)
		case *box.Box:
			return boxapi.DialContext(ctx, value, nil, network, addr)
		case nil:
			reference := activeCoreLifecycle.currentReference()
			return activeCoreLifecycle.dial(ctx, reference, network, addr)
		default:
			return nil, errInvalidCoreInstance
		}
	}
	neko_common.DialUDP = func(ctx context.Context, specifiedInstance interface{}) (net.PacketConn, error) {
		return nil, errDirectUDPFallbackDisabled
	}
	neko_common.CreateProxyHttpClient = func(specifiedInstance interface{}) *http.Client {
		switch value := specifiedInstance.(type) {
		case *coreInstanceReference:
			return generationBoundHTTPClient(value)
		case *box.Box:
			return boxapi.CreateProxyHttpClient(value, nil)
		case nil:
			return generationBoundHTTPClient(activeCoreLifecycle.currentReference())
		default:
			return rejectedHTTPClient(errInvalidCoreInstance)
		}
	}
}
