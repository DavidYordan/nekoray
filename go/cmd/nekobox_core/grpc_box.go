package main

import (
	"context"
	"errors"
	"fmt"

	"grpc_server"
	"grpc_server/gen"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/matsuridayo/libneko/speedtest"
	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/boxapi"

	"log"

	"github.com/sagernet/sing-box/option"
)

type server struct {
	grpc_server.BaseServer
}

func (s *server) Start(ctx context.Context, in *gen.LoadConfigReq) (out *gen.ErrorResp, _ error) {
	var err error

	defer func() {
		out = &gen.ErrorResp{}
		if err != nil {
			out.Error = err.Error()
			instance = nil
		}
	}()

	if neko_common.Debug {
		log.Println("Start:", in.CoreConfig)
	}

	if instance != nil {
		err = errors.New("instance already started")
		return
	}

	instance, instance_cancel, err = CreateSingBox([]byte(in.CoreConfig), nekoPlatformWriter{})

	if instance != nil {
		instance_stats = nil
		if in.StatsOutbounds != nil {
			instance_stats = boxapi.NewSbStatsService(option.V2RayStatsServiceOptions{
				Enabled:   true,
				Outbounds: in.StatsOutbounds,
			})
			if instance_stats != nil {
				instance.Router().AppendTracker(instance_stats)
			}
		}
	}

	return
}

func (s *server) Stop(ctx context.Context, in *gen.EmptyReq) (out *gen.ErrorResp, _ error) {
	var err error

	defer func() {
		out = &gen.ErrorResp{}
		if err != nil {
			out.Error = err.Error()
		}
	}()

	if instance == nil {
		return
	}

	instance_cancel()
	instance.Close()

	instance = nil
	instance_stats = nil

	return
}

func (s *server) Test(ctx context.Context, in *gen.TestReq) (out *gen.TestResp, _ error) {
	var err error
	out = &gen.TestResp{Ms: 0}

	defer func() {
		if err != nil {
			out.Error = err.Error()
		}
	}()

	if in.Mode == gen.TestMode_UrlTest {
		var i *box.Box
		var cancel context.CancelFunc
		if in.Config != nil {
			// Test instance
			i, cancel, err = CreateSingBox([]byte(in.Config.CoreConfig), nekoPlatformWriter{})
			if i != nil {
				defer i.Close()
				defer cancel()
			}
			if err != nil {
				return
			}
		} else {
			// Test running instance
			i = instance
			if i == nil {
				return
			}
		}
		// Latency
		out.Ms, err = speedtest.UrlTest(boxapi.CreateProxyHttpClient(i, nil), in.Url, in.Timeout, speedtest.UrlTestStandard_RTT)
	} else if in.Mode == gen.TestMode_TcpPing {
		out.Ms, err = speedtest.TcpPing(in.Address, in.Timeout)
	} else if in.Mode == gen.TestMode_FullTest {
		i, cancel, err := CreateSingBox([]byte(in.Config.CoreConfig), nekoPlatformWriter{})
		if i != nil {
			defer i.Close()
			defer cancel()
		}
		if err != nil {
			return
		}
		return grpc_server.DoFullTest(ctx, in, i)
	}

	return
}

func (s *server) QueryStats(ctx context.Context, in *gen.QueryStatsReq) (out *gen.QueryStatsResp, _ error) {
	out = &gen.QueryStatsResp{}

	if instance_stats != nil {
		out.Traffic, _ = instance_stats.GetStats(context.TODO(), fmt.Sprintf("outbound>>>%s>>>traffic>>>%s", in.Tag, in.Direct), true)
	}

	return
}

func (s *server) ListConnections(ctx context.Context, in *gen.EmptyReq) (*gen.ListConnectionsResp, error) {
	out := &gen.ListConnectionsResp{
		// TODO upstream api
	}
	return out, nil
}
