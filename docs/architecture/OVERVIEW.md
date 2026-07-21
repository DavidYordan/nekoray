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
  -> TransitionCoordinator + CoreProcess daemon generation
  -> 本地 token gRPC
  -> nekobox_core.exe / RouteFluent patched sing-box
  -> Mixed / internal TUN / outbound
```

当前 GUI 同时拥有主 core和网络数据面生命周期。Go wrapper仍只有整 Box 的 Start/Stop；Stop关闭内部 TUN、Mixed、DNS、outbounds、Wintun和动态 WFP session。这一结构不能满足 TUN下 fail-closed切线。无 instance 时的 system TCP/UDP/HTTP helper fallback 已封死，Start/Stop/dial/stats/Exit 也已由 process-local lifecycle mutex 和 generation-bound reference 串行，但持久保护层尚不存在。

普通 GUI 只连接 core 的 localhost gRPC，token 每个 GUI session 随机生成并经 stdin 传给 core；同一 GUI 会话内 daemon 重启沿用该 token，没有 token 仍不能调用 RPC。`Start`/`Stop`/`Exit` 另带该 GUI session 内单调递增的 command sequence；Go lifecycle 在检查状态前记录最高序号，因此同一 daemon 中较新的 Stop/Exit 能拒绝随后抢到锁的旧 Start。该序号仍不是 expected daemon generation，新 daemon 会接受它收到的首个有效序号，不能证明具体哪一代处理了请求。HTTP/2 client 的 30 秒 abort 只界定 GUI 等待，Go handler 不保证随 context 中止，也没有 indeterminate 后状态查询/对账，因此这不是端到端 deadline，token 和 command sequence 都不能充当跨 daemon fence。`nekobox_core run/check` 则是用户显式执行的高级 CLI，构建和隔离测试工具依赖它直接读取配置。当前 Go 层会执行 sing-box 自身的配置与进程内生命周期处理，但没有再次执行 C++ ConfigBuilder 的产品策略，因此这是一项 core 边界纵深防御缺口；它不等于普通 GUI 可以任意绕过 guard。

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
  -> immutable candidate bytes / config SHA-256 / managed-TUN flag / candidate daemon generation
  -> CoreProcess daemon generation（atomic queue-or-ready / one-shot request / crash timer fencing）
  -> gRPC（session-local monotonic command sequence）
  -> Go coreLifecycle mutex
       -> candidate 成功后发布 generation
       -> generation-bound dial / HTTP / stats
       -> Stop/Close 不确定 => BLOCKED，拒绝继续使用或重试
```

这套基础封住旧的跨线程解锁、Start/Stop/CrashCleanup 重叠、旧 completion 清除新 owner 的 mutation gate、同一 daemon 内旧 Start 晚于新 Stop 执行、旧 crash timer/ready event 操作新 daemon、旧 HTTP/reference 跨代使用等直接竞态。transition 所有权由 ticket 管理；legacy gRPC `Call` 的完成通知则改用 `QSemaphore` release/acquire，不再由不同线程 lock/unlock 同一个 `QMutex`。pending crash cleanup 由当前 transition 连续 handoff；旧 generation 未能明确 Stop 时，新 Start 不再继续。Start 不确定或 ack 后 UI commit 丢失 readiness 时，以本地 `max(candidate, current)` daemon generation 作为保守清理上界；RPC 没有 expected generation，不能证明实际接收者，该上界也可能保留过久。TrafficLooper 只在成功 commit 后发布。core 崩溃只重建空控制 core。详见 [ADR 0010](decisions/0010-process-local-lifecycle-generation-fencing.md)。

它不是持久 runtime：所有 owner 都仍在 GUI/子进程边界内，父进程死亡会带走当前数据面；没有 Windows Service、stable Mixed/TUN anchor、persistent WFP、完整 desired/observed/owner/health 状态机或 RPC indeterminate 对账。final Start gate 也不会重建 candidate/比较完整 model revision；RPC 未携带 expected daemon generation，前后 readiness 检查不能原子绑定服务端处理者。`core_running`/`prepare_exit` 已原子化；只有收到响应 daemon 已接受且以更高 command sequence 排在旧 Start 之后的 Stop，退出链才继续，但该响应不证明 daemon generation。30 秒 client abort 后仍会保守留在 indeterminate，Go handler 可能继续执行，跨 daemon 状态仍无法确认，尚无端到端 deadline 或 QProcess/GUI crash→commit/退出及真实超时集成测试。

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
