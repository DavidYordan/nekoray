package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"log"
	"net"
	"strconv"
	"strings"

	"grpc_server"
	"grpc_server/auth"
	"grpc_server/gen"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/matsuridayo/libneko/speedtest"

	"nekobox_core/internal/boxapi"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

const lifecycleCommandSequenceHeader = "nekoray_command_sequence"

type server struct {
	grpc_server.BaseServer
}

func lifecycleCommandSequence(ctx context.Context) (uint64, error) {
	md, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return 0, errors.New("lifecycle command metadata is missing")
	}
	values := md.Get(lifecycleCommandSequenceHeader)
	if len(values) != 1 {
		return 0, fmt.Errorf("lifecycle command sequence header count is %d, expected 1", len(values))
	}
	sequence, err := strconv.ParseUint(values[0], 10, 64)
	if err != nil || sequence == 0 {
		return 0, fmt.Errorf("invalid lifecycle command sequence %q", values[0])
	}
	return sequence, nil
}

// Exit intentionally overrides the promoted unimplemented RPC. This core must
// never terminate while an active or indeterminately closed generation is
// still retained.
func (s *server) Exit(ctx context.Context, in *gen.EmptyReq) (*gen.LifecycleStateResp, error) {
	commandSequence, err := lifecycleCommandSequence(ctx)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if !s.GracefulShutdownConfigured() {
		return nil, status.Error(codes.FailedPrecondition, "graceful shutdown is not configured")
	}
	snapshot, err := activeCoreLifecycle.requestExit(commandSequence)
	if err != nil {
		return nil, status.Error(codes.FailedPrecondition, err.Error())
	}
	response := lifecycleSnapshotToProto(ctx, snapshot)
	if !s.RequestGracefulShutdown() {
		return nil, status.Error(codes.Internal, "graceful shutdown request was not accepted")
	}
	return response, nil
}

func buildCoreCandidate(in *gen.LoadConfigReq) (*managedCore, error) {
	if in == nil {
		return nil, errors.New("start request is missing")
	}

	candidateBox, cancel, createErr := createSingBoxCandidate([]byte(in.CoreConfig), nekoPlatformWriter{})
	if candidateBox == nil {
		return nil, createErr
	}

	var candidateStats *boxapi.SbStatsService
	candidate := &managedCore{
		cancel: cancel,
		close:  candidateBox.Close,
		dial: func(ctx context.Context, network string, address string) (net.Conn, error) {
			return boxapi.DialContext(ctx, candidateBox, candidateStats, network, address)
		},
		queryStats: func(ctx context.Context, name string, reset bool) (int64, error) {
			if candidateStats == nil {
				return 0, nil
			}
			return candidateStats.GetStats(ctx, name, reset)
		},
	}
	if createErr != nil {
		return candidate, createErr
	}

	if in.StatsOutbounds != nil {
		candidateStats = boxapi.NewSbStatsService(boxapi.StatsServiceOptions{
			Enabled:   true,
			Outbounds: in.StatsOutbounds,
		})
		if candidateStats != nil {
			candidateBox.Router().AppendTracker(candidateStats)
		}
	}
	return candidate, nil
}

func (s *server) Start(ctx context.Context, in *gen.LoadConfigReq) (out *gen.ErrorResp, _ error) {
	out = &gen.ErrorResp{}
	if neko_common.Debug && in != nil {
		log.Println("Start:", in.CoreConfig)
	}
	commandSequence, err := lifecycleCommandSequence(ctx)
	if err != nil {
		out.Error = err.Error()
		return
	}
	configSHA256 := ""
	if in != nil {
		configSHA256 = fmt.Sprintf("%x", sha256.Sum256([]byte(in.CoreConfig)))
	}
	if err := activeCoreLifecycle.start(commandSequence, configSHA256, func() (*managedCore, error) {
		return buildCoreCandidate(in)
	}); err != nil {
		out.Error = err.Error()
	}
	return
}

func lifecycleCommandKindFromProto(kind gen.LifecycleCommandKind) coreLifecycleCommandKind {
	switch kind {
	case gen.LifecycleCommandKind_LIFECYCLE_COMMAND_START:
		return coreLifecycleCommandStart
	case gen.LifecycleCommandKind_LIFECYCLE_COMMAND_STOP:
		return coreLifecycleCommandStop
	case gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT:
		return coreLifecycleCommandExit
	default:
		return coreLifecycleCommandUnspecified
	}
}

func lifecyclePhaseToProto(phase coreLifecyclePhase) gen.LifecyclePhase {
	switch phase {
	case coreLifecycleStopped:
		return gen.LifecyclePhase_LIFECYCLE_PHASE_STOPPED
	case coreLifecycleStarting:
		return gen.LifecyclePhase_LIFECYCLE_PHASE_STARTING
	case coreLifecycleActive:
		return gen.LifecyclePhase_LIFECYCLE_PHASE_ACTIVE
	case coreLifecycleStopping:
		return gen.LifecyclePhase_LIFECYCLE_PHASE_STOPPING
	case coreLifecycleBlocked:
		return gen.LifecyclePhase_LIFECYCLE_PHASE_BLOCKED
	case coreLifecycleExiting:
		return gen.LifecyclePhase_LIFECYCLE_PHASE_EXITING
	default:
		return gen.LifecyclePhase_LIFECYCLE_PHASE_UNSPECIFIED
	}
}

func lifecycleCommandKindToProto(kind coreLifecycleCommandKind) gen.LifecycleCommandKind {
	switch kind {
	case coreLifecycleCommandStart:
		return gen.LifecycleCommandKind_LIFECYCLE_COMMAND_START
	case coreLifecycleCommandStop:
		return gen.LifecycleCommandKind_LIFECYCLE_COMMAND_STOP
	case coreLifecycleCommandExit:
		return gen.LifecycleCommandKind_LIFECYCLE_COMMAND_EXIT
	default:
		return gen.LifecycleCommandKind_LIFECYCLE_COMMAND_UNSPECIFIED
	}
}

func lifecycleCommandOutcomeToProto(outcome coreLifecycleCommandOutcome) gen.LifecycleCommandOutcome {
	switch outcome {
	case coreLifecycleOutcomeSucceeded:
		return gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_SUCCEEDED
	case coreLifecycleOutcomeFailedClean:
		return gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_FAILED_CLEAN
	case coreLifecycleOutcomeRejectedClean:
		return gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_REJECTED_CLEAN
	case coreLifecycleOutcomeBlocked:
		return gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_BLOCKED
	case coreLifecycleOutcomeFencedNotAdmitted:
		return gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED
	case coreLifecycleOutcomeSupersededUnknown:
		return gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_SUPERSEDED_UNKNOWN
	default:
		return gen.LifecycleCommandOutcome_LIFECYCLE_OUTCOME_UNSPECIFIED
	}
}

func lifecycleSnapshotToProto(
	ctx context.Context,
	snapshot coreLifecycleSnapshot,
) *gen.LifecycleStateResp {
	return &gen.LifecycleStateResp{
		DaemonInstanceId:           auth.DaemonInstanceID(ctx),
		OrderingWatermark:          snapshot.orderingWatermark,
		Phase:                      lifecyclePhaseToProto(snapshot.phase),
		RuntimeGeneration:          snapshot.runtimeGeneration,
		HasCurrent:                 snapshot.hasCurrent,
		ActiveStartCommandSequence: snapshot.activeStartCommandSequence,
		CurrentConfigSha256:        snapshot.currentConfigSHA256,
		TargetCommand: &gen.LifecycleCommandRecord{
			Sequence:               snapshot.targetCommand.sequence,
			Kind:                   lifecycleCommandKindToProto(snapshot.targetCommand.kind),
			Outcome:                lifecycleCommandOutcomeToProto(snapshot.targetCommand.outcome),
			ConfigSha256:           snapshot.targetCommand.configSHA256,
			RequestedConfigMatches: snapshot.targetConfigMatches,
		},
	}
}

func (s *server) ReconcileLifecycle(
	ctx context.Context,
	in *gen.LifecycleReconcileReq,
) (*gen.LifecycleStateResp, error) {
	if in == nil {
		return nil, status.Error(codes.InvalidArgument, "lifecycle reconcile request is missing")
	}
	targetKind := lifecycleCommandKindFromProto(in.TargetCommandKind)
	switch targetKind {
	case coreLifecycleCommandExit:
		// Exit is a daemon-lifecycle command and never names a runtime
		// configuration. Its empty hash is part of the protocol contract.
		if in.TargetConfigSha256 != "" {
			return nil, status.Error(codes.InvalidArgument, "Exit reconciliation must not include a target config SHA-256")
		}
	case coreLifecycleCommandStart, coreLifecycleCommandStop:
		if len(in.TargetConfigSha256) != sha256.Size*2 ||
			in.TargetConfigSha256 != strings.ToLower(in.TargetConfigSha256) {
			return nil, status.Error(codes.InvalidArgument, "target config SHA-256 must be lowercase hexadecimal")
		}
		if _, err := hex.DecodeString(in.TargetConfigSha256); err != nil {
			return nil, status.Error(codes.InvalidArgument, "target config SHA-256 must be lowercase hexadecimal")
		}
	default:
		return nil, status.Error(codes.InvalidArgument, "unsupported lifecycle reconciliation target kind")
	}
	barrierSequence, err := lifecycleCommandSequence(ctx)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	snapshot, err := activeCoreLifecycle.reconcile(
		barrierSequence,
		in.TargetCommandSequence,
		targetKind,
		in.TargetConfigSha256,
	)
	if err != nil {
		return nil, status.Error(codes.FailedPrecondition, err.Error())
	}
	return lifecycleSnapshotToProto(ctx, snapshot), nil
}

func (s *server) Stop(ctx context.Context, in *gen.EmptyReq) (out *gen.ErrorResp, _ error) {
	out = &gen.ErrorResp{}
	commandSequence, err := lifecycleCommandSequence(ctx)
	if err != nil {
		out.Error = err.Error()
		return
	}
	if err := activeCoreLifecycle.stop(commandSequence); err != nil {
		out.Error = err.Error()
	}
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
		if in.Config == nil {
			err = errors.New("URL Test requires an explicit bounded test configuration")
			return
		}
		// Test instance
		i, cancel, createErr := CreateSingBox([]byte(in.Config.CoreConfig), nekoPlatformWriter{})
		err = createErr
		if i != nil {
			defer i.Close()
			defer cancel()
		}
		if err != nil {
			return
		}
		// Latency
		out.Ms, err = speedtest.UrlTest(boxapi.CreateProxyHttpClient(i, nil), in.Url, in.Timeout, speedtest.UrlTestStandard_RTT)
	} else if in.Mode == gen.TestMode_TcpPing {
		err = errors.New("TCP Ping is disabled because it opens a direct system socket instead of using the selected outbound")
	} else if in.Mode == gen.TestMode_FullTest {
		if in.Config == nil {
			err = errors.New("Full Test requires an explicit bounded test configuration")
			return
		}
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
	if in == nil {
		return out, nil
	}
	traffic, err := activeCoreLifecycle.queryStats(
		ctx,
		fmt.Sprintf("outbound>>>%s>>>traffic>>>%s", in.Tag, in.Direct),
		true,
	)
	if errors.Is(err, boxapi.ErrNoActiveInstance) {
		return out, nil
	}
	if errors.Is(err, errCoreLifecycleBlocked) || errors.Is(err, errCoreLifecycleExiting) {
		return nil, status.Error(codes.FailedPrecondition, err.Error())
	}
	if err == nil {
		out.Traffic = traffic
	}
	return
}

func (s *server) ListConnections(ctx context.Context, in *gen.EmptyReq) (*gen.ListConnectionsResp, error) {
	out := &gen.ListConnectionsResp{
		// TODO upstream api
	}
	return out, nil
}
