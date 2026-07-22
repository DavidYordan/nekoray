package grpc_server

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"grpc_server/auth"
	"grpc_server/gen"
	"log"
	"net"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/matsuridayo/libneko/neko_common"

	grpc_auth "github.com/grpc-ecosystem/go-grpc-middleware/auth"
	"google.golang.org/grpc"
)

type BaseServer struct {
	gen.UnimplementedLibcoreServiceServer
	gracefulShutdownMu       sync.Mutex
	gracefulShutdownOnce     sync.Once
	gracefulShutdownCallback func()
}

const LifecycleProtocolVersion uint32 = 2

func (s *BaseServer) GetDaemonInfo(ctx context.Context, in *gen.EmptyReq) (*gen.DaemonInfoResp, error) {
	return &gen.DaemonInfoResp{
		DaemonInstanceId:         auth.DaemonInstanceID(ctx),
		LifecycleProtocolVersion: LifecycleProtocolVersion,
	}, nil
}

// ConfigureGracefulShutdown binds this service instance to the gRPC server
// that owns it. It is configured exactly once before Serve starts.
func (s *BaseServer) ConfigureGracefulShutdown(callback func()) bool {
	if callback == nil {
		return false
	}
	s.gracefulShutdownMu.Lock()
	defer s.gracefulShutdownMu.Unlock()
	if s.gracefulShutdownCallback != nil {
		return false
	}
	s.gracefulShutdownCallback = callback
	return true
}

func (s *BaseServer) GracefulShutdownConfigured() bool {
	s.gracefulShutdownMu.Lock()
	defer s.gracefulShutdownMu.Unlock()
	return s.gracefulShutdownCallback != nil
}

// RequestGracefulShutdown starts the callback outside the Exit handler. A
// synchronous GracefulStop would wait for that same handler and deadlock.
func (s *BaseServer) RequestGracefulShutdown() bool {
	s.gracefulShutdownMu.Lock()
	callback := s.gracefulShutdownCallback
	s.gracefulShutdownMu.Unlock()
	if callback == nil {
		return false
	}
	s.gracefulShutdownOnce.Do(func() {
		go callback()
	})
	return true
}

func RunCore(setupCore func(), server gen.LibcoreServiceServer) {
	_token := flag.String("token", "", "")
	_port := flag.Int("port", 19810, "")
	_debug := flag.Bool("debug", false, "")
	_instanceID := flag.String("instance-id", "", "")
	flag.CommandLine.Parse(os.Args[2:])

	neko_common.Debug = *_debug

	go func() {
		parent, err := os.FindProcess(os.Getppid())
		if err != nil {
			log.Fatalln("find parent:", err)
		}
		if runtime.GOOS == "windows" {
			state, err := parent.Wait()
			log.Fatalln("parent exited:", state, err)
		} else {
			for {
				time.Sleep(time.Second * 10)
				err = parent.Signal(syscall.Signal(0))
				if err != nil {
					log.Fatalln("parent exited:", err)
				}
			}
		}
	}()

	// Libcore
	setupCore()

	// GRPC
	lis, err := net.Listen("tcp", "127.0.0.1:"+strconv.Itoa(*_port))
	if err != nil {
		log.Fatalf("failed to listen: %v", err)
	}

	token := *_token
	if token == "" {
		os.Stderr.WriteString("Please set a token: ")
		s := bufio.NewScanner(os.Stdin)
		if s.Scan() {
			token = strings.TrimSpace(s.Text())
		}
	}
	if token == "" {
		fmt.Println("You must set a token")
		os.Exit(0)
	}
	instanceID := strings.TrimSpace(*_instanceID)
	if instanceID == "" {
		log.Fatalln("daemon instance id is required")
	}
	os.Stderr.WriteString("token is set\n")

	auther := auth.Authenticator{
		Token:            token,
		DaemonInstanceID: instanceID,
	}

	grpcServer := grpc.NewServer(
		grpc.StreamInterceptor(grpc_auth.StreamServerInterceptor(auther.Authenticate)),
		grpc.UnaryInterceptor(grpc_auth.UnaryServerInterceptor(auther.Authenticate)),
	)
	shutdownController, ok := server.(interface {
		ConfigureGracefulShutdown(func()) bool
	})
	if !ok || !shutdownController.ConfigureGracefulShutdown(grpcServer.GracefulStop) {
		log.Fatalln("core service does not provide a one-shot graceful shutdown controller")
	}
	gen.RegisterLibcoreServiceServer(grpcServer, server)

	name := "nekobox_core"

	log.Printf("%s grpc server listening at %v\n", name, lis.Addr())
	if err := grpcServer.Serve(lis); err != nil {
		log.Fatalf("failed to serve: %v", err)
	}
}
