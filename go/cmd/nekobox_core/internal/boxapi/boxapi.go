package boxapi

import (
	"context"
	"errors"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/common/dialer"
	"github.com/sagernet/sing/common/bufio"
	E "github.com/sagernet/sing/common/exceptions"
	"github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
)

var ErrNoActiveInstance = errors.New("no active sing-box instance; direct network fallback is disabled")

type SbStatsService struct {
	createdAt time.Time
	inbounds  map[string]bool
	outbounds map[string]bool
	users     map[string]bool
	access    sync.Mutex
	counters  map[string]*atomic.Int64
}

var _ adapter.ConnectionTracker = (*SbStatsService)(nil)

type StatsServiceOptions struct {
	Enabled   bool
	Inbounds  []string
	Outbounds []string
	Users     []string
}

func NewSbStatsService(options StatsServiceOptions) *SbStatsService {
	if !options.Enabled {
		return nil
	}
	inbounds := make(map[string]bool)
	outbounds := make(map[string]bool)
	users := make(map[string]bool)
	for _, inbound := range options.Inbounds {
		inbounds[inbound] = true
	}
	for _, outbound := range options.Outbounds {
		outbounds[outbound] = true
	}
	for _, user := range options.Users {
		users[user] = true
	}
	return &SbStatsService{
		createdAt: time.Now(),
		inbounds:  inbounds,
		outbounds: outbounds,
		users:     users,
		counters:  make(map[string]*atomic.Int64),
	}
}

func DialContext(ctx context.Context, instance *box.Box, tracker adapter.ConnectionTracker, network, addr string) (net.Conn, error) {
	if instance == nil {
		return nil, ErrNoActiveInstance
	}
	defOutboundTag := instance.Outbound().Default().Tag()
	conn, err := dialer.NewDetour(instance.Outbound(), defOutboundTag, true).DialContext(ctx, network, metadata.ParseSocksaddr(addr))
	if err != nil {
		return nil, err
	}
	if stats, ok := tracker.(*SbStatsService); ok && stats != nil {
		conn = stats.RoutedConnectionInternal("", defOutboundTag, "", conn, false)
	}
	return conn, nil
}

func CreateProxyHttpClient(instance *box.Box, tracker adapter.ConnectionTracker) *http.Client {
	transport := &http.Transport{
		TLSHandshakeTimeout:   time.Second * 3,
		ResponseHeaderTimeout: time.Second * 3,
	}

	transport.DialContext = func(ctx context.Context, network, addr string) (net.Conn, error) {
		return DialContext(ctx, instance, tracker, network, addr)
	}

	return &http.Client{
		Transport: transport,
	}
}

func (s *SbStatsService) RoutedConnection(ctx context.Context, conn net.Conn, metadata adapter.InboundContext, matchedRule adapter.Rule, matchOutbound adapter.Outbound) net.Conn {
	inbound := metadata.Inbound
	user := metadata.User
	outbound := matchOutbound.Tag()
	return s.RoutedConnectionInternal(inbound, outbound, user, conn, true)
}

func (s *SbStatsService) RoutedConnectionInternal(inbound string, outbound string, user string, conn net.Conn, directIn bool) net.Conn {
	var readCounter []*atomic.Int64
	var writeCounter []*atomic.Int64
	countInbound := inbound != "" && s.inbounds[inbound]
	countOutbound := outbound != "" && s.outbounds[outbound]
	countUser := user != "" && s.users[user]
	if !countInbound && !countOutbound && !countUser {
		return conn
	}
	s.access.Lock()
	if countInbound {
		readCounter = append(readCounter, s.loadOrCreateCounter("inbound>>>"+inbound+">>>traffic>>>uplink"))
		writeCounter = append(writeCounter, s.loadOrCreateCounter("inbound>>>"+inbound+">>>traffic>>>downlink"))
	}
	if countOutbound {
		readCounter = append(readCounter, s.loadOrCreateCounter("outbound>>>"+outbound+">>>traffic>>>uplink"))
		writeCounter = append(writeCounter, s.loadOrCreateCounter("outbound>>>"+outbound+">>>traffic>>>downlink"))
	}
	if countUser {
		readCounter = append(readCounter, s.loadOrCreateCounter("user>>>"+user+">>>traffic>>>uplink"))
		writeCounter = append(writeCounter, s.loadOrCreateCounter("user>>>"+user+">>>traffic>>>downlink"))
	}
	s.access.Unlock()
	if directIn {
		return bufio.NewInt64CounterConn(conn, readCounter, writeCounter)
	}
	return bufio.NewInt64CounterConn(conn, writeCounter, readCounter)
}

func (s *SbStatsService) RoutedPacketConnection(ctx context.Context, conn N.PacketConn, metadata adapter.InboundContext, matchedRule adapter.Rule, matchOutbound adapter.Outbound) N.PacketConn {
	inbound := metadata.Inbound
	user := metadata.User
	outbound := matchOutbound.Tag()
	var readCounter []*atomic.Int64
	var writeCounter []*atomic.Int64
	countInbound := inbound != "" && s.inbounds[inbound]
	countOutbound := outbound != "" && s.outbounds[outbound]
	countUser := user != "" && s.users[user]
	if !countInbound && !countOutbound && !countUser {
		return conn
	}
	s.access.Lock()
	if countInbound {
		readCounter = append(readCounter, s.loadOrCreateCounter("inbound>>>"+inbound+">>>traffic>>>uplink"))
		writeCounter = append(writeCounter, s.loadOrCreateCounter("inbound>>>"+inbound+">>>traffic>>>downlink"))
	}
	if countOutbound {
		readCounter = append(readCounter, s.loadOrCreateCounter("outbound>>>"+outbound+">>>traffic>>>uplink"))
		writeCounter = append(writeCounter, s.loadOrCreateCounter("outbound>>>"+outbound+">>>traffic>>>downlink"))
	}
	if countUser {
		readCounter = append(readCounter, s.loadOrCreateCounter("user>>>"+user+">>>traffic>>>uplink"))
		writeCounter = append(writeCounter, s.loadOrCreateCounter("user>>>"+user+">>>traffic>>>downlink"))
	}
	s.access.Unlock()
	return bufio.NewInt64CounterPacketConn(conn, readCounter, nil, writeCounter, nil)
}

func (s *SbStatsService) GetStats(ctx context.Context, name string, reset bool) (int64, error) {
	s.access.Lock()
	counter, loaded := s.counters[name]
	s.access.Unlock()
	if !loaded {
		return 0, E.New(name, " not found.")
	}
	if reset {
		return counter.Swap(0), nil
	}
	return counter.Load(), nil
}

//nolint:staticcheck
func (s *SbStatsService) loadOrCreateCounter(name string) *atomic.Int64 {
	counter, loaded := s.counters[name]
	if loaded {
		return counter
	}
	counter = &atomic.Int64{}
	s.counters[name] = counter
	return counter
}
