# Windows Sandbox 隔离验证

状态：环境已安装，等待宿主人工重启后完成首次运行
最后更新：2026-08-31

## 目的与边界

Windows Sandbox 是当前首选的 Windows 隔离验证入口，用于承载不应在依赖 Clash TUN 的开发主机上执行的 GUI、Wintun 和项目 TUN 基本生命周期检查。它不是 Linux/WSL 替代层，也不把 Sandbox 的一次性系统等同于真实 Windows 安装、休眠、多网卡或物理网卡验收。

宿主 Clash TUN 是永久 no-touch 资源。创建、运行和清理 Sandbox 不得停止、重启、结束或改写 Clash，也不得修改宿主系统代理、DNS、路由、网卡、Hyper-V virtual switch 或其它网络状态。当前 Sandbox 配置固定使用 `<Networking>Disable</Networking>`；因此第一阶段只能验证离线 Windows 行为，不能用它访问真实节点或证明远端线路可用。

## 当前安装状态

2026-08-31 只读盘点确认：宿主为 Windows 10 Pro 19045 x64，固件虚拟化和现有 hypervisor 可用，`VirtualMachinePlatform` 已启用；`Microsoft-Hyper-V-All` 未启用。随后以管理员权限执行：

```powershell
Enable-WindowsOptionalFeature -Online `
  -FeatureName Containers-DisposableClientVM -All -NoRestart
```

Windows 返回 feature `Enabled` 且 `RestartNeeded=True`。本轮没有自动重启，因为重启会中断用户明确要求持续运行的 Clash TUN。重启前 `WindowsSandbox.exe` 尚不可用，这是 OS feature servicing 尚未完成，不是脚本或 NekoRay 错误。用户在合适时间正常保存工作并手工重启 Windows 后，无需再次安装该 feature。

微软的系统要求和 `.wsb` 配置说明见：

- <https://learn.microsoft.com/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-install>
- <https://learn.microsoft.com/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-configure-using-wsb-file>

## 仓库工具与隔离模型

`tools/prepare_windows_sandbox.ps1` 每次创建新的、被 Git 忽略的 `artifacts/windows-sandbox/<run-id>/`，拒绝覆盖旧运行。生成的 `.wsb` 具有以下固定约束：

- 网络、vGPU、音频/视频输入、打印机和剪贴板重定向全部禁用；
- 只把新建的 staging input 映射为 `C:\NekoRayInput`，且是只读；
- 只把该次新建的 results 映射为 `C:\NekoRayResults`，且可写；
- 不映射仓库根或 `deployment/windows64`；
- 指定 package 时只复制 GUI/core、Qt DLL/plugin、geodata 和明确 manifest 白名单；`config`、groups、routes、recovery、日志、转储、压缩包及其它顶层内容不会进入 Sandbox；
- runner 只结束自己启动且仍持有对象句柄的精确进程，不按名称、端口或模糊条件清理。

Sandbox runner 首先记录 Windows 版本、管理员身份和 adapter 状态，再计算只读输入 SHA-256。若 staging 中有 package，它把白名单副本复制到 Sandbox 内的可写工作目录，只运行 `nekobox_core.exe version` 和隔离空 appdata 的 `nekobox.exe -flag_config_transaction_report`；不会启动 profile、TUN、系统代理或网络探测。结果必须由宿主 `verify_windows_sandbox_results.ps1` 对输入哈希、`Networking=Disable` 下无 Up adapter 和进程退出码再次核验。

## 重启后的首次运行

先只验证 Sandbox 自身，不把旧 deployment 冒充当前源码 package：

```powershell
$preparation = .\tools\prepare_windows_sandbox.ps1 | ConvertFrom-Json
Start-Process -FilePath $preparation.wsb_path
```

等待 Sandbox 内脚本完成并在 results 目录生成 `completed.marker`，然后在宿主验证：

```powershell
.\tools\verify_windows_sandbox_results.ps1 `
  -RunRoot $preparation.run_root -Json
```

取得同一 commit、无 Skip 的当前 Windows package 后，再新建一次运行；不得复用旧 staging：

```powershell
$preparation = .\tools\prepare_windows_sandbox.ps1 `
  -PackageDir .\deployment\windows64 | ConvertFrom-Json
Start-Process -FilePath $preparation.wsb_path

.\tools\verify_windows_sandbox_results.ps1 `
  -RunRoot $preparation.run_root -Json
```

`-PackageDir` 只用于本轮已核对 provenance 的干净 package。工具对白名单外顶层内容的跳过不能把一个混有真实配置的目录变成发布输入，也不能替代 `BUILD_WINDOWS.md` 的同轮构建和哈希要求。

## 后续验收顺序

1. 首次 offline Sandbox 启动、输入哈希和无网络 adapter 通过；
2. 同一 commit package 的 core version 与 GUI maintenance 命令通过；
3. 增加只操作 Sandbox 自身的 Wintun 创建/销毁、项目 TUN 启停和精确接口清理用例；
4. 只有离线生命周期不足以回答产品问题时，另行设计隔离 Windows VM 的网络拓扑并提交用户审核。不得为了真实线路把当前 `.wsb` 改成默认 networking，也不得自动创建/修改宿主 vSwitch。

前三层仍不能证明真实远端线路、系统代理对物理应用、休眠恢复、驱动安装持久性或多网卡行为。每层报告都必须明确写 `isolated-windows` 及实际覆盖范围。
