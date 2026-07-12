package main

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"syscall"

	"github.com/matsuridayo/libneko/neko_log"
	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/experimental/deprecated"
	"github.com/sagernet/sing-box/include"
	sblog "github.com/sagernet/sing-box/log"
	"github.com/sagernet/sing-box/option"
	"github.com/sagernet/sing/common/json"
	"github.com/sagernet/sing/common/json/badjson"
	"github.com/sagernet/sing/service"
)

type nekoPlatformWriter struct{}

func (nekoPlatformWriter) DisableColors() bool {
	return true
}

func (nekoPlatformWriter) WriteMessage(level sblog.Level, message string) {
	if neko_log.LogWriter == nil {
		return
	}
	_, _ = neko_log.LogWriter.Write([]byte(message + "\n"))
}

type configEntry struct {
	content []byte
	path    string
	options option.Options
}

func singBoxContext() context.Context {
	ctx := service.ContextWith(context.Background(), deprecated.NewStderrManager(sblog.StdLogger()))
	return include.Context(ctx)
}

func readConfigAt(ctx context.Context, path string) (*configEntry, error) {
	var (
		content []byte
		err     error
	)
	if path == "stdin" {
		content, err = io.ReadAll(os.Stdin)
	} else {
		content, err = os.ReadFile(path)
	}
	if err != nil {
		return nil, fmt.Errorf("read config at %s: %w", path, err)
	}
	options, err := json.UnmarshalExtendedContext[option.Options](ctx, content)
	if err != nil {
		return nil, fmt.Errorf("decode config at %s: %w", path, err)
	}
	return &configEntry{
		content: content,
		path:    path,
		options: options,
	}, nil
}

func readConfig(ctx context.Context, paths []string, directories []string) (option.Options, error) {
	if len(paths) == 0 && len(directories) == 0 {
		paths = append(paths, "config.json")
	}

	var entries []*configEntry
	for _, path := range paths {
		entry, err := readConfigAt(ctx, path)
		if err != nil {
			return option.Options{}, err
		}
		entries = append(entries, entry)
	}
	for _, directory := range directories {
		files, err := os.ReadDir(directory)
		if err != nil {
			return option.Options{}, fmt.Errorf("read config directory at %s: %w", directory, err)
		}
		for _, file := range files {
			if file.IsDir() || !strings.HasSuffix(file.Name(), ".json") {
				continue
			}
			entry, err := readConfigAt(ctx, filepath.Join(directory, file.Name()))
			if err != nil {
				return option.Options{}, err
			}
			entries = append(entries, entry)
		}
	}

	sort.Slice(entries, func(i, j int) bool {
		return entries[i].path < entries[j].path
	})
	if len(entries) == 1 {
		return entries[0].options, nil
	}

	var merged json.RawMessage
	var err error
	for _, entry := range entries {
		merged, err = badjson.MergeJSON(ctx, entry.options.RawMessage, merged, false)
		if err != nil {
			return option.Options{}, fmt.Errorf("merge config at %s: %w", entry.path, err)
		}
	}
	var mergedOptions option.Options
	err = mergedOptions.UnmarshalJSONContext(ctx, merged)
	if err != nil {
		return option.Options{}, fmt.Errorf("unmarshal merged config: %w", err)
	}
	return mergedOptions, nil
}

func createSingBoxWithOptions(ctx context.Context, options option.Options, platformWriter sblog.PlatformWriter) (*box.Box, context.CancelFunc, error) {
	if options.Log == nil {
		options.Log = &option.LogOptions{}
	}
	options.Log.DisableColor = true

	ctx, cancel := context.WithCancel(ctx)
	instance, err := box.New(box.Options{
		Context:           ctx,
		Options:           options,
		PlatformLogWriter: platformWriter,
	})
	if err != nil {
		cancel()
		return nil, nil, fmt.Errorf("create service: %w", err)
	}
	err = instance.Start()
	if err != nil {
		cancel()
		_ = instance.Close()
		return nil, nil, fmt.Errorf("start service: %w", err)
	}
	return instance, cancel, nil
}

func checkSingBoxWithOptions(ctx context.Context, options option.Options) error {
	if options.Log == nil {
		options.Log = &option.LogOptions{}
	}
	options.Log.DisableColor = true

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	instance, err := box.New(box.Options{
		Context: ctx,
		Options: options,
	})
	if err != nil {
		return err
	}
	err = instance.PreStart()
	if err != nil {
		return err
	}
	return instance.Close()
}

func CreateSingBox(config []byte, platformWriter sblog.PlatformWriter) (*box.Box, context.CancelFunc, error) {
	ctx := singBoxContext()
	options, err := json.UnmarshalExtendedContext[option.Options](ctx, config)
	if err != nil {
		return nil, nil, err
	}
	return createSingBoxWithOptions(ctx, options, platformWriter)
}

func createSingBoxFromFiles(paths []string, directories []string) (*box.Box, context.CancelFunc, error) {
	ctx := singBoxContext()
	options, err := readConfig(ctx, paths, directories)
	if err != nil {
		return nil, nil, err
	}
	return createSingBoxWithOptions(ctx, options, nil)
}

func checkSingBoxFromFiles(paths []string, directories []string) error {
	ctx := singBoxContext()
	options, err := readConfig(ctx, paths, directories)
	if err != nil {
		return err
	}
	return checkSingBoxWithOptions(ctx, options)
}

func runSingBoxCLI(args []string) {
	if len(args) == 0 {
		printCoreUsage()
		os.Exit(2)
	}

	switch args[0] {
	case "version", "-v", "--version":
		return
	case "check":
		paths, directories, workingDir, err := parseConfigArgs(args[1:])
		if err != nil {
			exitWithError(err)
		}
		if workingDir != "" {
			if err = os.Chdir(workingDir); err != nil {
				exitWithError(err)
			}
		}
		if err = checkSingBoxFromFiles(paths, directories); err != nil {
			exitWithError(err)
		}
	case "run":
		paths, directories, workingDir, err := parseConfigArgs(args[1:])
		if err != nil {
			exitWithError(err)
		}
		if workingDir != "" {
			if err = os.Chdir(workingDir); err != nil {
				exitWithError(err)
			}
		}
		runSingBox(paths, directories)
	default:
		printCoreUsage()
		os.Exit(2)
	}
}

func runSingBox(paths []string, directories []string) {
	osSignals := make(chan os.Signal, 1)
	signal.Notify(osSignals, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	defer signal.Stop(osSignals)

	for {
		instance, cancel, err := createSingBoxFromFiles(paths, directories)
		if err != nil {
			exitWithError(err)
		}
		osSignal := <-osSignals
		cancel()
		if err = instance.Close(); err != nil {
			fmt.Fprintln(os.Stderr, err)
		}
		if osSignal != syscall.SIGHUP {
			return
		}
	}
}

func parseConfigArgs(args []string) (paths []string, directories []string, workingDir string, err error) {
	for i := 0; i < len(args); i++ {
		arg := args[i]
		next := func() (string, error) {
			i++
			if i >= len(args) {
				return "", fmt.Errorf("missing value for %s", arg)
			}
			return args[i], nil
		}
		switch {
		case arg == "-c" || arg == "--config":
			value, nextErr := next()
			if nextErr != nil {
				return nil, nil, "", nextErr
			}
			paths = append(paths, value)
		case strings.HasPrefix(arg, "--config="):
			paths = append(paths, strings.TrimPrefix(arg, "--config="))
		case arg == "-C" || arg == "--config-directory":
			value, nextErr := next()
			if nextErr != nil {
				return nil, nil, "", nextErr
			}
			directories = append(directories, value)
		case strings.HasPrefix(arg, "--config-directory="):
			directories = append(directories, strings.TrimPrefix(arg, "--config-directory="))
		case arg == "-D" || arg == "--directory":
			workingDir, err = next()
			if err != nil {
				return nil, nil, "", err
			}
		case strings.HasPrefix(arg, "--directory="):
			workingDir = strings.TrimPrefix(arg, "--directory=")
		case arg == "--disable-color":
		case arg == "-h" || arg == "--help":
			printCoreUsage()
			os.Exit(0)
		default:
			return nil, nil, "", fmt.Errorf("unknown argument: %s", arg)
		}
	}
	return
}

func exitWithError(err error) {
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}

func printCoreUsage() {
	fmt.Fprintln(os.Stderr, "Usage: nekobox_core nekobox <port> <token>")
	fmt.Fprintln(os.Stderr, "       nekobox_core run -c <config>")
	fmt.Fprintln(os.Stderr, "       nekobox_core check -c <config>")
	fmt.Fprintln(os.Stderr, "       nekobox_core version")
}
