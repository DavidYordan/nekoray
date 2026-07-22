# 架构总览

## 产品边界

本项目不是 sing-box-only 重写。Qt UI、ProfileManager、NekoRay 路由/导入/测速/外置 core能力继续构成产品主体；RouteFluent patched sing-box 是主核心，用于承载 AnyTLS和 server-domain DoH扩展。Xray核心删除，其它 NekoRay能力默认保留。

## 当前主核心链路

```text
Qt Widgets UI
  -> DataStore / ProfileManager
  -> ConfigBuilder
       -> 主 Mixed 12080 -> 主 chain
       -> 辅助 Mixed port -> 对应辅助 chain
       -> Clash proxy-server-nameserver -> server domain resolver
  -> TransitionCoordinator + CoreProcess {generation, daemon UUID, PID}
  -> 本地 token + UUID gRPC / ready handshake / reconcile barrier / Exit ACK
  -> nekobox_core.exe / RouteFluent patched sing-box
  -> Mixed / internal TUN / outbound
```

当前 GUI 同时拥有主 core和网络数据面生命周期。数据面变更仍只有整 Box 的 Start/Stop，没有 Reload/Prepare/Commit；Stop关闭内部 TUN、Mixed、DNS、outbounds、Wintun和动态 WFP session。`GetDaemonInfo` 与 `ReconcileLifecycle` 只提供进程身份/状态对账，不改变这一数据面限制。这一结构不能满足 TUN下 fail-closed切线。无 instance 时的 system TCP/UDP/HTTP helper fallback 已封死，Start/Stop/dial/stats/Exit 也已由 process-local lifecycle mutex 和 generation-bound reference 串行，但持久保护层尚不存在。

普通 GUI 只连接 core 的 localhost gRPC，token 每个 GUI session 随机生成并经 stdin 传给 core；同一 GUI 会话内 daemon 重启沿用该 token，没有 token 仍不能调用 RPC。每次 QProcess 启动另生成不可变 UUID，所有 RPC 在 handler 前验证该精确身份；日志只触发 `GetDaemonInfo` 身份/协议 v2 握手，不能独自构成 ready。`Start`/`Stop`/`Exit` 另带单调 command sequence；Go lifecycle 在同一 mutex 内记录最高序号。Start/Stop 响应不确定时，更高序号 `ReconcileLifecycle` barrier 会等待目标终态或先封住迟到目标，并返回 target outcome、config hash 与当前 phase。Exit 只允许精确 `STOPPED` 原子进入 `EXITING`，先返回结构化 ACK，再由 one-shot `GracefulStop` 结束 server；active/blocked 不隐式 Stop。GUI 冻结 `{generation, UUID, PID}` 并持续等同一 QProcess `NormalExit/0`，不 kill/replacement。ACK 丢失时，只有更高序号对账精确证明 `STOPPED/FENCED_NOT_ADMITTED` 才恢复控制，否则保持 continuation fence 并继续等 finished。30 秒 Start/Stop abort 与各次对账/Exit ACK 的 client 时限仍只界定单次 GUI 等待，Go handler/屏障不保证随 context 中止；再次超时仍为 unknown，所以这不是通用端到端 deadline。`nekobox_core run/check` 则是用户显式执行的高级 CLI，构建和隔离测试工具依赖它直接读取配置。当前 Go 层会执行 sing-box 自身的配置与进程内生命周期处理，但没有再次执行 C++ ConfigBuilder 的产品策略，因此这是一项 core 边界纵深防御缺口；它不等于普通 GUI 可以任意绕过 guard。

当前安全隔离会拒绝任何 inbound `set_system_proxy=true` 和未由产品 TUN 开关生成的 TUN；受管 TUN 必须与完整生成对象一致，并保留生成的接口自动检测、不得出现 default/bind-interface 覆盖。系统 WireGuard/Tailscale endpoint 与 NTP 写系统时钟也被拒绝，`internal-full` 则与产品 TUN、辅助并发和测试路径隔离。顶层 custom merge 另以完整 listener/outbound 快照保护受管 Mixed。上述均是配置时窄 guard，不代表任意路由已证明安全，也没有改变 GUI 退出会带走 worker 的架构事实。

GUI 已分别展示 TUN 的 requested state 和当前 worker-observed state，并可显示已提交 transition generation/config hash；受管 TUN observed 值来自最终通过校验的 candidate。core 崩溃后只重启空控制 core，不自动恢复 profile/TUN。worker 观测不是 Windows 接口、路由或 WFP 的事实源，持久保护层仍不存在。

Windows GUI 现忽略 CRT `SIGTERM`/`SIGINT`，避免控制台信号调用 Qt 并绕过受保护退出路径。强制进程终止、崩溃、系统关机与父 GUI 消失仍会绕过或摧毁当前数据面；该窄止损不改变持久 Runtime/WFP 的必要性。

旧 Windows 外置 TUN launcher 无法获得可验证的 worker PID/句柄，停止时曾按映像名清理。接管工作树已禁止该启停路径但保留其数据模型，避免误伤其它安装；后续由 Runtime Service 精确接管，而不是把 NekoRay 能力静默删除。

NekoRay external-core能力被上一阶段误删，恢复后应重新接在 ConfigBuilder/进程管理边界：普通上游模式继续可用；无法加入单 Box并发的组合在并发托管模式中明确拒绝，而不是删除协议。

## 当前进程内生命周期基础

```text
GUI TransitionCoordinator（Start/Stop/CrashCleanup 单一 ticket）
  -> coordinator mutex 内同步 participating-mutation depth gate
  -> pending crash cleanup 连续 handoff
  -> immutable candidate bytes / config SHA-256 / managed-TUN flag / candidate daemon UUID
  -> CoreProcess {generation, UUID, PID}（日志触发握手 / atomic queue-or-ready / crash/finished fencing）
  -> gRPC v2（token + UUID；session-local monotonic command sequence / reconcile barrier / Exit ACK）
  -> Go coreLifecycle mutex
       -> candidate 成功后发布 generation
       -> generation-bound dial / HTTP / stats
       -> Stop/Close 不确定 => BLOCKED，拒绝继续使用或重试
       -> 精确 STOPPED 才能 EXITING -> ACK -> GracefulStop
```

这套基础封住旧的跨线程解锁、Start/Stop/CrashCleanup 重叠、旧 completion 清除新 owner 的 mutation gate、旧 Start 晚于新 Stop、旧 crash timer/ready event 操作新 daemon、旧 HTTP/reference 跨代使用，以及 reused port/token 把旧请求落到新 daemon 等直接竞态。transition 所有权由 ticket 管理；gRPC 完成通知使用 `QSemaphore`；每代 daemon 使用 UUID fence；pending crash cleanup 连续 handoff。Start/Stop 响应丢失时只在同一 UUID 的排序屏障给出精确 target/config/phase 结论后推进，否则保留 indeterminate。Exit 则冻结 QProcess generation/UUID/PID：精确 ACK 后只等该进程 finished；ACK 不可用时也只有 `STOPPED/FENCED_NOT_ADMITTED` 对账能取消，否则持续等待。重复退出请求不能绕过 continuation fence，生产代码不 kill 或替换该 daemon。TrafficLooper 只在成功 commit 后发布，core 崩溃只重建空控制 core。详见 [ADR 0010](decisions/0010-process-local-lifecycle-generation-fencing.md) 与 [ADR 0011](decisions/0011-daemon-identity-and-lifecycle-reconciliation.md)。

它不是持久 runtime：所有 owner 都仍在 GUI/子进程边界内，父进程死亡会带走当前数据面；没有 Windows Service、stable Mixed/TUN anchor、persistent WFP、完整 desired/observed/owner/health 状态机或 Windows OS 事实源。final Start gate 也不会重建 candidate/比较完整 model revision。UUID/对账和 Exit ACK/finished 只证明一个 GUI-owned core 进程内的 lifecycle/进程结果；不证明线路健康、Mixed/TUN/WFP 或路由无泄漏。client abort/对账再次超时后 Go 操作仍可能继续，`GracefulStop` 也可能等待长时间在途 RPC；真正 context-aware 的单 owner command executor 仍缺。完整 package 的 raw QProcess/Qt HTTP/2 gate 使用无 listener、无 TUN 配置且不调用产品 Client/MainWindow，因此仍缺 GUI→Client、crash→commit、ACK 丢失注入、父进程死亡与 Windows 资源集成测试。

## 候选 Windows 运行时

```text
Qt GUI（控制面，可退出/重启）
  -> authenticated named pipe
Windows Runtime Service（稳定所有者）
  -> RuntimeStateMachine / generation journal
  -> 独立 persistent WFP fail-closed policy
  -> 稳定 TUN/Mixed anchor
  -> A/B sing-box / external-core outbound generations
  -> user-session system-proxy broker（仅手动命令）
```

候选目标不是为了增加平台，而是实现冻结语义：GUI生命周期不改变OS模式；线路切换先保护、后准备/健康检查、再提交；失败全阻断而不直连。详见 [ADR 0008](decisions/0008-persistent-windows-runtime.md)。

## 状态与所有权

- GUI保存用户配置和显式命令，不把意图当作 observed state。
- Runtime Service/状态机串行化运行变更并持有精确进程句柄、路径、创建时间、generation和配置hash。
- Windows OS是系统代理、接口、路由和WFP状态的事实源。
- ProfileManager mutation与订阅提交必须串行；文件保存必须原子。ConfigBuilder 的统计 tag/profile id 与 VLESS flow 已不再回写 live model，group speedtest 也改用 UI immutable job/fingerprint CAS，但完整 `BuildModelSnapshot` 仍未完成。`TrafficData::last_update` 已初始化为 `0`；共享 counter/rate 的 worker 写入与 UI/JsonStore 读取仍需独立遥测快照或同步协议，generation-local `TrafficBinding` 不等于统计线程安全。
- 外部生产 `D:\Program Files\nekoray` 永远不属于本状态机。

## 主要目录

- `main/`：启动、DataStore、通用基础设施。
- `db/`：profile/group、配置构建、路由与并发映射。
- `fmt/`：协议 Bean、分享链接、core outbound。
- `sub/`：订阅导入/刷新；未经授权的 MultiMapper 与复杂批量 resolver/change-IP 平台已移除。旧 Resolve domain 的系统 DNS/永久改 IP 实现也已删除，UI 入口仅保留无副作用禁用说明。
- `ui/`：Qt控制面。
- `rpc/`、`go/grpc_server/`：C++/Go RPC边界。
- `go/cmd/nekobox_core/`：patched sing-box wrapper。
- `third_party/routefluent-sing-box/`：受控补丁构建。
- `tools/`、`test/`：只读诊断与隔离临时探针。
