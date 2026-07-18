package main

import (
	"io/ioutil"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

func main() {
	// update & launcher
	exe, err := os.Executable()
	if err != nil {
		panic(err.Error())
	}

	wd := filepath.Dir(exe)
	os.Chdir(wd)
	exe = filepath.Base(os.Args[0])
	log.Println("exe:", exe, "exe dir:", wd)
	updateArgs, restartArgs := splitUpdaterArgs(os.Args[1:])

	if strings.HasPrefix(strings.ToLower(exe), "updater") {
		if runtime.GOOS == "windows" {
			if strings.HasPrefix(strings.ToLower(exe), "updater.old") {
				// 2. "updater.old" update files
				time.Sleep(time.Second)
				Updater(updateArgs)
				// 3. start
				exec.Command("./nekobox.exe", restartArgs...).Start()
			} else {
				// 1. main prog quit and run "updater.exe"
				Copy("./updater.exe", "./updater.old")
				exec.Command("./updater.old", os.Args[1:]...).Start()
			}
		} else {
			// 1. update files
			Updater(updateArgs)
			// 2. start
			if os.Getenv("NKR_FROM_LAUNCHER") == "1" {
				args := append([]string{"--"}, restartArgs...)
				exec.Command("./launcher", args...).Start()
			} else {
				exec.Command("./nekobox", restartArgs...).Start()
			}
		}
		return
	} else if strings.HasPrefix(strings.ToLower(exe), "launcher") {
		Launcher()
		return
	}
	log.Fatalf("wrong name")
}

func splitUpdaterArgs(args []string) ([]string, []string) {
	for i, arg := range args {
		if arg == "--" {
			return args[:i], args[i+1:]
		}
	}
	return args, nil
}

func Copy(src string, dst string) {
	// Read all content of src to data
	data, _ := ioutil.ReadFile(src)
	// Write data to dst
	ioutil.WriteFile(dst, data, 0644)
}
