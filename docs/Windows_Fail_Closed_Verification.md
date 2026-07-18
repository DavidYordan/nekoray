# Windows 无偷跑验证

本文记录本项目在 Windows 上验证系统代理/TUN fail-closed 行为的方法。

验证脚本：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_fail_closed_restart.ps1
```

默认模式只做一次只读快照，不会关闭任何进程，也不会修改系统代理、路由或 TUN。

## 快照检查

在仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_fail_closed_restart.ps1
```

脚本会读取：

- `deployment/windows64/config/groups/nekobox.json` 中的主 mixed 端口和 Clash API 端口。
- Windows 当前用户系统代理注册表：`HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings`。
- WinHTTP proxy。
- 本项目部署目录 `deployment/windows64` 下的进程。
- 生产目录 `D:\Program Files\nekoray` 下的进程，仅用于记录，不会停止。
- 主端口/Clash API 端口监听。
- TUN/TAP/Wintun/WireGuard/Neko 相关网卡和 IPv4 默认路由。

报告默认写入：

```text
deployment/windows64/fail_closed_audit/fail_closed_<timestamp>.json
```

该目录在 `deployment/` 下，不进入 Git。

## 重启窗口监控

用于验证“重启期间不清空系统代理”：

1. 启动 `deployment/windows64/nekobox.exe`。
2. 启动一条线路，并按要验证的场景开启系统代理或 TUN。
3. 在仓库根目录运行监控：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_fail_closed_restart.ps1 -MonitorSeconds 45 -PollMilliseconds 250 -StrictProjectProxy
```

4. 在 45 秒窗口内，在 GUI 中执行 `Restart Program`、`Restart Proxy` 或更新器重启流程。
5. 脚本结束后查看 `OK` / `FAILED` 以及 JSON 报告。

如果基线时系统代理已开启并指向项目主端口，监控期间出现以下情况会判失败：

- `ProxyEnable` 从 `1` 变成 `0`。
- `ProxyServer` 被改成其它值。
- `AutoConfigURL` 被改动。
- 使用 `-StrictProjectProxy` 时，系统代理不再指向项目主端口。

端口短时没有监听不算失败。重启期间本机端口断开属于 fail-closed，关键是不能把 Windows 系统代理恢复成直连。

## TUN 监控

外部 TUN：

- 如果基线检测到本项目目录下的 `nekobox_core.exe ... sing-box-vpn.json`，脚本会记录它是外部 TUN core。
- 监控期间该进程消失会判失败，因为自动重启/更新不应主动停止外部 TUN。

内部 TUN：

- 当前策略不是热加载，而是阻断会卸载内部 TUN 的隐式 stop/start。
- 验证内部 TUN 场景时，应先启动内部 TUN，再尝试 `Restart Program`、`Restart Proxy`、切换线路或辅助端口变更；预期 GUI 提示需要先显式关闭 TUN，脚本报告中不应出现系统代理或路由被撤销。
- 如需要严格检查 TUN 相关网卡和默认路由不变化，可增加 `-ExpectTunStable`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_fail_closed_restart.ps1 -MonitorSeconds 45 -PollMilliseconds 250 -ExpectTunStable
```

如果本机同时运行生产 Nekoray TUN，默认不要加 `-ExpectTunStable`，否则生产 TUN 自身的路由波动也会被判为本次采样失败。

## 常用参数

- `-MonitorSeconds 45`：从基线开始轮询 45 秒。
- `-PollMilliseconds 250`：每 250 毫秒采样一次。
- `-ExpectedProxyPort 12080`：手动指定项目系统代理端口；默认从配置读取 `inbound_socks_port`。
- `-StrictProjectProxy`：要求基线系统代理必须指向项目主端口。
- `-ExpectTunStable`：要求采样窗口内 TUN 类网卡和 IPv4 默认路由保持稳定。
- `-Json`：仅输出 JSON，便于后续自动化消费。
- `-OutputPath <path>`：指定报告文件。

## 结果解释

- `OK`：未检测到 fail-open 变化。
- `FAILED`：检测到系统代理被清空、代理服务器被改写、外部 TUN core 消失，或启用了 `-ExpectTunStable` 后路由/网卡变化。
- `Warnings`：表示这次采样没有覆盖某些场景，例如基线系统代理是关闭的、没有检测到项目外部 TUN。这类告警不代表代码失败，但说明本轮验证不能证明对应路径。
