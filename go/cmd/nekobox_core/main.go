package main

import (
	"fmt"
	"os"
	_ "unsafe"

	"grpc_server"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/sagernet/sing-box/constant"
)

func main() {
	printVersion()

	// nekobox_core
	if len(os.Args) > 1 && os.Args[1] == "nekobox" {
		fmt.Println()
		neko_common.RunMode = neko_common.RunMode_NekoBox_Core
		grpc_server.RunCore(setupCore, &server{})
		return
	}

	// sing-box
	runSingBoxCLI(os.Args[1:])
}

func printVersion() {
	fmt.Println("sing-box:", constant.Version, "NekoBox:", neko_common.Version_neko)
}
