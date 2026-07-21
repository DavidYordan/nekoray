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

var instance *box.Box
var instance_cancel context.CancelFunc
var instance_stats *boxapi.SbStatsService

func setupCore() {
	neko_log.SetupLog(50*1024, "./neko.log")
	//
	neko_common.GetCurrentInstance = func() interface{} {
		return instance
	}
	neko_common.DialContext = func(ctx context.Context, specifiedInstance interface{}, network, addr string) (net.Conn, error) {
		if specifiedInstance != nil {
			if i, ok := specifiedInstance.(*box.Box); ok {
				return boxapi.DialContext(ctx, i, nil, network, addr)
			}
			return nil, errInvalidCoreInstance
		}
		if instance != nil {
			return boxapi.DialContext(ctx, instance, instance_stats, network, addr)
		}
		return nil, boxapi.ErrNoActiveInstance
	}
	neko_common.DialUDP = func(ctx context.Context, specifiedInstance interface{}) (net.PacketConn, error) {
		return nil, errDirectUDPFallbackDisabled
	}
	neko_common.CreateProxyHttpClient = func(specifiedInstance interface{}) *http.Client {
		if specifiedInstance != nil {
			if i, ok := specifiedInstance.(*box.Box); ok {
				return boxapi.CreateProxyHttpClient(i, nil)
			}
			return boxapi.CreateProxyHttpClient(nil, nil)
		}
		return boxapi.CreateProxyHttpClient(instance, instance_stats)
	}
}
