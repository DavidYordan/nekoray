# Windows fail-closed 验证

状态：需求已确认，实现尚未验收
最后更新：2026-07-22

## 已确认的产品语义

- 启用或停用系统代理、启用或停用 TUN，只能由用户发起的明确、精准操作执行。
- GUI 退出、GUI 重启、应用更新和线路重启都不得顺带恢复、重写或切换系统代理/TUN 状态。
- TUN 开启时必须允许线路重启；不得要求用户先关闭 TUN。若切换期无法保持代理通路，必须 fail-closed 阻断而不能直连；是否允许短暂黑洞和既有连接重置仍待确认。
- 停止操作只能作用于本项目拥有且身份已核对的对象。`D:\Program Files\nekoray`、`2080` 和生产 TUN 永远不在测试或清理范围内。

当前实现还不能声称满足这些要求。标准内部 TUN 已不再采用旧 `vpn_strict_route`/`vpn_ipv6` 开关决定保护范围，而是强制 `strict_route=true` 并生成 IPv4/IPv6 地址；最终配置还锁定完整 TUN 对象和已知接口/OS 副作用字段。但 GUI 生命周期与 core/TUN 生命周期仍耦合，现有 WFP dynamic session 会随 worker 消失，因此这些配置时止损不能证明物理 IPv6、多宿主 DNS 或崩溃窗口安全。无 instance 时的 system TCP/UDP/HTTP helper fallback 已封死，也只消除一个直连路径。

## requested 与 observed 状态

- `spmode_vpn` 是用户请求的期望状态；复选框是否勾选不能证明 TUN 已创建。
- `running_internal_tun` 或外置 worker PID 是当前 worker 的观测。UI 会区分 **worker active**、**requested; inactive** 和 **ACTIVE; stop incomplete**，但这仍不是 Windows 接口、路由、DNS 或 WFP 的事实源。
- core 崩溃清理会把 worker 观测置为 inactive，只重启空控制 core，并保留 requested 意图而不自动恢复 profile/TUN。这符合“不得隐式启用”，但实际 TUN/动态保护会随 worker 消失，故仍不合格。
- 正常退出、重启、切线和辅助端口变更在内部 TUN worker 活动时仍被 UI guard 阻止。它避免主动制造已知直连窗口，却违反“开 TUN 时可顺利切线/退出且 OS 模式不变”的最终需求。
- Windows GUI 已忽略 CRT `SIGTERM`/`SIGINT`，这两个控制台信号不会再绕过 UI guard；该行为不覆盖任务管理器/`TerminateProcess`、崩溃、系统关机或 core 自退，不能据此减少故障注入矩阵。

## 只读快照

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_fail_closed_restart.ps1 `
  -PackageDir <package-dir>
```

脚本默认只做一次只读快照；只有显式设置非零 `-MonitorSeconds` 才会连续监控。它不会修改系统状态。即使输出“未检测到 fail-open”，也只表示采样窗口内没有观察到有限规则覆盖的变化，不能证明 WFP、IPv6、适配器、外置 TUN 接管或崩溃恢复正确。

## 验收场景

每个场景都必须同时采集 Windows API/注册表、路由表、DNS、适配器、监听 PID/可执行路径和真实联网结果：

1. 系统代理和 TUN 关闭时，GUI 退出、重启和线路切换不得启用它们。
2. 系统代理和 TUN 开启时，GUI 退出、重启和线路切换不得停用、恢复或改写它们。
3. TUN 开启时重启当前线路，保护规则先于旧 core 停止，并保持到新 core 通过健康检查；故障时继续阻断，不得自动切回旧线路。只有用户明确选择且旧 generation 重新验证后才执行显式回滚，任何阶段都不得 direct 泄漏。
4. core 启动失败、崩溃、卡死、提权失败、端口占用、DNS 失败和 IPv4/IPv6 分别故障时，观察结果必须与“无偷跑”一致；同时核对 requested、worker-observed 与 Windows 实际状态，不得把“请求仍为开”当作 TUN 已恢复。
5. worker 和 Runtime Service 分别终止、BFE 重启、NIC 切换及休眠恢复时，persistent WFP 仍阻断未授权物理 IPv4/IPv6/DNS；helper 不得改走系统网络。
6. 用户手动关闭某项网络能力时，只精准撤销本项目仍拥有的状态；检测到外部修改时不得覆盖第三方当前设置。
7. 任意停止、回滚和清理只能命中本次测试创建、且 PID/路径/创建时间均已核对的进程与资源。

线路重启是否允许短暂黑洞、是否必须保留既有 TCP/UDP 会话，以及 Windows 重启后如何维持保护并等待用户手动重新启用 TUN，仍需作为单独产品决策记录；不得提供预授权自动启用 TUN。这些体验问题不改变“不得 direct 泄漏”和“不得隐式切换系统网络”的硬要求。

## 测试环境与维护窗口

本机生产 Nekoray 必须保持运行，因此日常自动测试只允许执行无侵入快照和隔离 contract。OpenWrt 可验证相同 core 的配置、DNS 和远端出站，但不能验证 Windows Wintun、系统代理、WFP、GUI 退出或线路重启；边界见 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md)。

2026-07-20 的历史 OpenWrt 探针每次都保持既有 RouteFluent PID `24565`、命令行、配置/manifest 哈希和监听集合完全不变，并清理了每个临时目录；但旧探针对临时配置强制 `auto_detect_interface=true`。这些结果只能证明资源所有权/清理约束以及同一接口变体下的组合诊断，必须按默认 preserve 重跑，且不能证明 Windows 产品已经 fail-closed；尤其不能替代 TUN 开启时的线路重启、core 崩溃、WFP 或 IPv4/IPv6 泄漏测试。

只有同时满足以下条件才申请维护窗口：问题属于 Windows 专有行为；独立 Windows 测试环境无法复现或不可用；生产 TUN 的独占驱动、接口或默认路由使证据无效；并且已准备好精确步骤、预期阻断行为和人工恢复方案。工具和 agent 不得自行停止生产 Nekoray。

这些条件全部实现并在受控 Windows 环境完成故障注入前，脚本输出只能作为审计证据，不能作为 fail-closed 通过证明。
