# ADR 0010：进程内生命周期串行化与 generation fencing

状态：已接受（阶段 3 基础）
日期：2026-07-22

## 背景

当前 GUI、`CoreProcess` 和 `nekobox_core` 都属于同一 GUI 生命周期。旧实现用两个 `QMutex` 分别防止 Start/Stop 重入，但锁可能由 UI 线程取得、worker 线程释放，既违反 mutex 所有权，也无法封住两次调用之间的空窗。core 崩溃后的延迟重启没有 daemon generation，旧 timer 可能作用于已经重建的新进程。Go wrapper 则用无锁全局 instance/cancel/stats，`Start`、`Stop`、dial 和 stats 可能并发访问不同生命周期状态。

这些问题会让 GUI 的 running/TUN 显示、core 中真实 generation 和迟到异步完成互相脱节。它们必须先止损，但仅靠进程内锁不能实现 [ADR 0008](0008-persistent-windows-runtime.md) 要求的持久数据面和无泄漏切换。

## 决策

### C++ GUI 与 daemon

1. GUI 用单一 `TransitionCoordinator` 为每次 Start/Stop/CrashCleanup 分配单调 generation ticket。同一时刻只允许一个 transition；只有持有当前 ticket 的完成回调可以释放它，迟到回调不能释放更新的一代。coordinator 在同一 mutex 临界区内同步 participating-mutation depth gate：`busy || crashCleanupPending` 时为 1，仅真正 quiescent 时为 0，因此失败获取或旧代完成不能把新 owner/pending cleanup 的 gate 写成 0。core crash 会先登记 pending cleanup；当前 transition 完成时直接把所有权原子 handoff 给新的 CrashCleanup generation，中间不暴露可被普通 Start/Stop 抢占的 idle 窗口。
2. Start 在读取恢复状态、group/TUN guard 和构建 candidate 之前取得 ticket。通过最终配置校验后，序列化 core config、SHA-256、连接统计选项、“candidate 是否包含受管内部 TUN”和**发送前本地捕获的** candidate daemon generation 均按值冻结；Start RPC 后不得再从可变 UI 设置反推该 generation 的 TUN 状态。RPC 协议没有 expected daemon generation，且 channel/token/port 会在同一 GUI session 的 daemon restart 间复用，因此本地 generation 只能用于保守记账，不能证明哪一代 daemon 实际接收了请求。最终 gate 只串行参与 mutation 的路径，并复核 recovery、当前 transition ticket 与已捕获 daemon readiness；它**不会**重建 candidate 或比较完整 model revision，不能证明构建期间所有模型读取仍与最终模型一致。
3. 旧 generation 未能明确 Stop 时，新 Start 中止。Start 请求出现应用错误、transport 失败，或本地 candidate daemon 在 acknowledgement 后、最终 UI commit 前失去 readiness 时，GUI 会把 candidate/profile/TUN 标为 indeterminate，并把清理归属保守记为 `max(candidateDaemonGeneration, CurrentDaemonGeneration)`。这样旧 daemon crash 不能过早清除一个可能跨 restart 落到新 daemon 的请求；代价是状态可能保留过久，直到该上界 generation crash 或显式 Stop 被确认。Stop RPC 失败或结果不确定时也保留原 observed profile/TUN 状态并显示 indeterminate，不把失败伪装成已停止。TrafficLooper 的统计 binding 只有最终 UI runtime commit 确认成功后才发布，失败/迟到 Start 不得发布遥测 generation。这些都只是 fail-closed 记账，没有证明实际 RPC receiver，也没有解决跨 daemon 对账。
4. `CoreProcess` 单独维护 daemon generation 和 profile-request generation。queue 与“daemon 已 ready”判断在同一状态锁内完成：尚未 ready 时排队，已经 ready 时立即返回 generation-bound one-shot request，关闭旧的 `IsDaemonReady(false)` 与排队之间恰逢 ready 而永久搁置的窗口。排队 profile、daemon ready event 与消费动作必须同时匹配两代 token；UI 先保留 ready 请求，busy 时等待当前 transition 排空，只有成功取得 Start ticket 后才一次消费。显式 Stop/退出、新的直接 user/reload Start 或 daemon stop 会取消排队或已经发出但尚未消费的旧请求。崩溃延迟 timer 必须同时匹配 generation 且进程仍为未运行，不能结束或重启后来的一代。
5. core 崩溃只允许重启**空控制 core**；不自动恢复 profile、TUN 或辅助线路。这样避免隐式改变用户模式，但当前数据面仍会消失，所以仍是发布阻断。
6. 跨 CoreProcess/UI/worker 使用的 `DataStore::core_running` 与 `prepare_exit` 改为 atomic bool；CoreProcess 不再跨线程读取 `spmode_vpn` 来选择恢复行为，所有异常退出统一只重建空控制 core。退出/重启必须取得 Stop completion：Stop 失败或不确定时不调用 core Exit、也不 quit GUI，而是在 UI 线程撤销 `prepare_exit`/save freeze、恢复 hotkey/control 并保留 observed runtime；只有响应 daemon 已接受且按序完成的 Stop 才继续退出。该响应仍不证明 daemon generation，也不会改变系统代理或 TUN。
7. legacy gRPC `Call` 不再用一个线程 lock、另一个线程 unlock 同一 `QMutex` 来通知完成，改为 worker `QSemaphore::release`、等待方 `acquire`，使响应写入经同步边可见。每个 `Client` 为 Start/Stop/Exit 生成单调 command sequence 并放入鉴权后的 gRPC metadata；Go lifecycle 在持锁后、检查 phase 前先推进最高序号。即使 handler 取得 mutex 的顺序与 GUI 发出顺序不同，较新的 Stop/Exit 也会让随后执行的旧 Start 以 stale sequence 失败。Start/Stop HTTP/2 client 另设置 30 秒 abort 上限，只界定 GUI 等待；超时视为 indeterminate，不能据此推断 Go handler 已停止或服务端状态未改变。

### Go core

1. 单一 lifecycle mutex 串行化 candidate 创建/发布、Stop、dial、stats 和 Exit 状态检查。Start/Stop/Exit 在 phase 检查前验证并记录 session-local command sequence；零值、重复或低于已接受序号的命令 fail closed。candidate 只有完整启动成功后才发布。
2. Stop 先使旧 generation reference 失效，再 cancel/close。candidate 清理或 active Close 失败属于不确定状态：保留不可使用的 blocked generation，拒绝新的 Start、dial、stats、重复 Stop 和 Exit，要求替换 daemon 进程，绝不把它当作干净 stopped。
3. 默认/current-core 的代理 HTTP client 和 dial 使用 generation-bound reference；旧 reference、stopped 时取得的 reference 或 blocked generation 均 fail closed。generation-bound HTTP transport 禁止连接复用，避免旧连接跨代存活。显式传入临时 `*box.Box` 的隔离 Test 路径仍由该请求自身持有，不应与持久 current generation 混写。
4. Exit 只在 lifecycle 精确为 stopped 时允许；它不会隐式 Stop active/blocked generation，因为 Stop 可能移除 TUN 或其它保护状态。

## 明确不包含的能力

本决策只约束**当前 GUI 和 core 进程内部**，不是 Windows fail-closed runtime 的完成方案：

- 没有独立于 GUI/父进程的 Windows Runtime Service；父进程死亡仍会带走 core 和当前数据面。
- 没有 `desired/observed/owner/health` 完整 `RuntimeStateMachine`、持久 generation journal 或精确 Windows 资源事实源。
- 没有稳定 Mixed/TUN anchor、A/B worker 切换、persistent WFP kill-switch 或 IPv4/IPv6/DNS 故障注入验收。
- 没有 `validate/prepare → protection active → start/health → commit / 用户显式选择且重验后的 rollback` 的 OS 级事务。
- 没有完整 `BuildModelSnapshot` 或 model revision CAS；final Start gate 不会重新构建/比较已冻结 candidate。
- gRPC token 每个 GUI session 随机生成，同一 GUI 内 daemon 重启会沿用；session-local command sequence 只排列一个 daemon 收到的 lifecycle 命令，`Start`/`Stop` 请求仍未携带 expected daemon generation。新 daemon 接受其收到的首个有效序号；C++ 在 RPC 前后检查 daemon readiness 只能缩小窗口，不能让具体 daemon generation 与服务端处理原子绑定。
- GUI Start/Stop HTTP/2 client 虽有 30 秒 abort，但没有服务端可取消的端到端 deadline 和“请求已送达但响应丢失”后的状态查询/对账协议。Go handler 未按该 context 中止生命周期操作，client 超时后服务端仍可能完成；现有 indeterminate 标记只保守阻断，不能确认请求实际作用于哪一代，也不能用超时后盲目重试解决。
- Go core 仍未独立重复 C++ ConfigBuilder 的全部 Mixed/TUN/系统代理/resolver 产品策略校验。
- Stop 的 client wait 会在 30 秒 abort 后返回不确定；退出链随后保持应用/observed runtime，不把超时自动转成父进程结束 core。服务端操作可能仍在继续且无法查询，这是安全止损，不是可恢复的退出协议。

因此现有 TUN reload/退出 guard 必须保留，产品仍为 Alpha/不可发布；不得把本 ADR 用作真实 Windows TUN/WFP、退出或无泄漏切线验收证据。

## 验证边界

- `test/runtime_transition_test.cpp` 覆盖 transition 单一 owner、跨线程完成、迟到完成不得释放新 generation/清除新代 depth gate、失败获取保留 owner/pending gate、pending crash cleanup 的连续 handoff/重复 crash 链，以及 daemon/profile-request generation、ready 后 queue 的 immediate one-shot 顺序用例、`MarkProcessReady`/Queue 真并发恰好一次、旧 crash timer、ready event 单次消费、显式取消、新请求覆盖旧事件、跨 daemon 失效和 cancel/consume 竞争。
- `go/cmd/nekobox_core/core_lifecycle_test.go` 覆盖失败 candidate 不发布、清理/Close 失败进入 blocked、旧 reference/HTTP client 不得跨代、dial/stats 与 Stop 互斥、并发 Start 只发布一次、较新 Stop 拒绝迟到旧 Start，以及 active/blocked lifecycle 拒绝 Exit；gRPC server 测试另拒绝缺失/零 command sequence 的 Exit。
- `go/cmd/nekobox_core/internal/boxapi/boxapi_test.go` 覆盖 generation-bound HTTP transport 禁止 keep-alive 复用。

这些是进程内状态机单元证据；仍没有真实 QProcess/GUI crash→commit、HTTP/2 client abort/服务端迟到完成集成、父进程死亡或 Windows TUN/WFP 测试。

## 后果与后续

进程内 Start/Stop 和 core instance 的直接竞态得到明确 fence，运行状态可附带 generation 与 config hash 供诊断；代价是 blocked/无响应场景会选择停止推进而不是自动 fallback。下一步仍按 ADR 0008 建立持久 RuntimeStateMachine、Windows Service、stable anchor 和 persistent WFP，再把当前 generation 机制迁移为受保护 OS 事务的一部分。
