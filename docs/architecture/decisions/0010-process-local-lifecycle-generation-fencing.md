# ADR 0010：进程内生命周期串行化与 generation fencing

状态：已接受（阶段 3 基础）
日期：2026-07-22

修订说明：本 ADR 的 GUI/core 串行化基础继续有效；跨 daemon 身份、ready、超时对账与 lifecycle v2 Exit 闭环已由 [ADR 0011](0011-daemon-identity-and-lifecycle-reconciliation.md) 收紧。下文已按现行实现更新，不得再使用本地 generation 上界替代 RPC receiver 身份。

## 背景

当前 GUI、`CoreProcess` 和 `nekobox_core` 都属于同一 GUI 生命周期。旧实现用两个 `QMutex` 分别防止 Start/Stop 重入，但锁可能由 UI 线程取得、worker 线程释放，既违反 mutex 所有权，也无法封住两次调用之间的空窗。core 崩溃后的延迟重启没有 daemon generation，旧 timer 可能作用于已经重建的新进程。Go wrapper 则用无锁全局 instance/cancel/stats，`Start`、`Stop`、dial 和 stats 可能并发访问不同生命周期状态。

这些问题会让 GUI 的 running/TUN 显示、core 中真实 generation 和迟到异步完成互相脱节。它们必须先止损，但仅靠进程内锁不能实现 [ADR 0008](0008-persistent-windows-runtime.md) 要求的持久数据面和无泄漏切换。

## 决策

### C++ GUI 与 daemon

1. GUI 用单一 `TransitionCoordinator` 为每次 Start/Stop/CrashCleanup 分配单调 generation ticket。同一时刻只允许一个 transition；只有持有当前 ticket 的完成回调可以释放它，迟到回调不能释放更新的一代。coordinator 在同一 mutex 临界区内同步 participating-mutation depth gate：`busy || crashCleanupPending` 时为 1，仅真正 quiescent 时为 0，因此失败获取或旧代完成不能把新 owner/pending cleanup 的 gate 写成 0。core crash 会先登记 pending cleanup；当前 transition 完成时直接把所有权原子 handoff 给新的 CrashCleanup generation，中间不暴露可被普通 Start/Stop 抢占的 idle 窗口。
2. Start 在读取恢复状态、group/TUN guard 和构建 candidate 之前取得 ticket。通过最终配置校验后，序列化 core config、SHA-256、连接统计选项、“candidate 是否包含受管内部 TUN”及发送前本地捕获的 `{candidate daemon generation, UUID}` 均按值冻结；Start RPC 后不得从可变 UI 设置或 current daemon 反推身份/TUN 状态。所有 RPC 在 handler 前验证 UUID，跨 restart 的新 daemon 会拒绝旧请求。最终 gate 只串行参与 mutation 的路径，并复核 recovery、当前 ticket 与已捕获 daemon readiness；它仍不会重建 candidate 或比较完整 model revision。
3. 旧 generation 未能明确 Stop 时，新 Start 中止。Start/Stop 出现应用错误或 transport 失败后，GUI 会向同一 UUID 发出更高 sequence 的 `ReconcileLifecycle` barrier；只有 target command、服务端 config hash、phase 与 active sequence 构成精确 active/stopped 结论时才收敛。这里的 active/stopped 只描述该 daemon 的进程内 `managedCore`，不是 Windows 接口、路由、Mixed、TUN 或 WFP 的事实。barrier 再次超时、blocked、superseded 或字段不一致时保留 observed profile/TUN 与 indeterminate，不把失败伪装成停止。TrafficLooper binding 仍只有最终 UI runtime commit 成功后才发布。
4. `CoreProcess` 单独维护 daemon generation、不可变 UUID 和 profile-request generation。stdout/stderr 日志只触发 `GetDaemonInfo`；只有回显 UUID/协议版本的结果被同一状态锁接受后才 ready。queue 与 ready 判断仍原子化；排队 profile、ready event 与消费动作必须匹配 generation/request token。daemon stop 会清除身份，迟到握手不能复活旧进程。旧进程未确认退出时不得发布 replacement UUID；崩溃 timer 也必须匹配 generation 与进程状态。
5. core 崩溃只允许重启**空控制 core**；不自动恢复 profile、TUN 或辅助线路。这样避免隐式改变用户模式，但当前数据面仍会消失，所以仍是发布阻断。
6. 跨 CoreProcess/UI/worker 使用的 `DataStore::core_running` 与 `prepare_exit` 为 atomic bool；异常退出统一只重建空控制 core。退出/重启必须先取得同一 UUID 的 Stop completion，或目标 outcome/phase/hash 均匹配的 daemon 内存态 stopped 对账；失败/unknown 时不发送 Exit、也不 quit GUI，而是恢复 UI 控制并保留 observed runtime。该 stopped 结论不是 Windows OS 清理证明。
7. lifecycle protocol v2 的 Exit 返回结构化 `LifecycleStateResp`。Go 只允许精确 `STOPPED` 且没有 current generation 的命令原子进入终态 `EXITING`，返回同一 daemon UUID、command sequence/watermark 和 `EXIT/SUCCEEDED` ACK；active、blocked、重复 Exit 及后续 Start/Stop/Reconcile 均拒绝，绝不隐式 Stop。ACK 生成后由一次性 callback 在 handler 外启动 gRPC `GracefulStop`，让当前响应先完成，daemon 再正常离开 Serve。
8. `CoreProcess` 在 `QProcess::started` 时冻结 `{generation, UUID, PID}`，在该 QProcess 的 `finished(exitCode, exitStatus)` 上记录结果；tracker 同时覆盖 finished 先于 wait 与 wait 先于 finished。GUI 收到精确 ACK 后保持 `prepare_exit`、保存和 UI fence，按 10 秒报告间隔持续等待同一三元组的 `NormalExit/0`；没有总等待超时，不 kill/terminate、不发布 replacement identity。重复菜单、托盘或窗口关闭请求由 continuation fence 拒绝，只有该异步 Stop/Exit 链可以授权最终 GUI 退出。
9. Exit ACK 不可用时，GUI 以更高 sequence 对同一 UUID 发 `ReconcileLifecycle(EXIT)`。只有精确 `STOPPED`、无 current、target outcome 为 `FENCED_NOT_ADMITTED` 且空 config hash 时，才能证明原 Exit 未被接纳并已被 barrier 封住，此时才恢复应用控制；任何超时、断连、malformed、superseded、active、blocked 或可能已进入 `EXITING` 的结果都保持 continuation fence，并持续等同一 `{generation, UUID, PID}` finished。该选择可能让 GUI 一直等待，但避免把已提交而 ACK 丢失的 Exit 当作失败后重启。
10. gRPC `Call` 使用 `QSemaphore` 建立跨线程完成同步。每个 `Client` 为 Start/Stop/Exit/Reconcile 生成单调 command sequence；Go lifecycle 在同一 mutex 内推进 watermark。`ReconcileLifecycle` 的更高序号会推进 watermark，但不会覆盖最后一条 lifecycle command 记录：目标先锁则 barrier 等待其终态，barrier 先锁则迟到目标因 stale sequence 被拒绝。它不是只读状态查询。30 秒 Start/Stop、5 秒普通对账以及 Exit ACK/对账的 client 时限都只界定单次 GUI RPC 等待；超时不会取消服务端 handler/barrier，不能据此推断服务端已停止，unknown 也不能盲目重试。

### Go core

1. 单一 lifecycle mutex 串行化 candidate 创建/发布、Stop、dial、stats 和 Exit 状态检查。Start/Stop/Exit 在 phase 检查前验证 command sequence、推进 session-local ordering watermark，并记录各自 lifecycle outcome；零值、重复或低于已接受序号的命令 fail closed。candidate 只有完整启动成功后才发布。
2. Stop 先使旧 generation reference 失效，再 cancel/close。candidate 清理或 active Close 失败属于不确定状态：保留不可使用的 blocked generation，拒绝新的 Start、dial、stats、重复 Stop 和 Exit，要求替换 daemon 进程，绝不把它当作干净 stopped。
3. 默认/current-core 的代理 HTTP client 和 dial 使用 generation-bound reference；旧 reference、stopped 时取得的 reference 或 blocked generation 均 fail closed。generation-bound HTTP transport 禁止连接复用，避免旧连接跨代存活。显式传入临时 `*box.Box` 的隔离 Test 路径仍由该请求自身持有，不应与持久 current generation 混写。
4. Exit 只在 lifecycle 精确为 stopped 时允许，并原子转入不可逆的 process-local `EXITING`；它不会隐式 Stop active/blocked generation，因为 Stop 可能移除 TUN 或其它保护状态。服务端以结构化 ACK 提交该状态，再由 one-shot `GracefulStop` 排空 gRPC server；同一 daemon 不再接纳任何新的 lifecycle 操作。

## 明确不包含的能力

本决策只约束**当前 GUI 和 core 进程内部**，不是 Windows fail-closed runtime 的完成方案：

- 没有独立于 GUI/父进程的 Windows Runtime Service；父进程死亡仍会带走 core 和当前数据面。
- 没有 `desired/observed/owner/health` 完整 `RuntimeStateMachine`、持久 generation journal 或精确 Windows 资源事实源。
- 没有稳定 Mixed/TUN anchor、A/B worker 切换、persistent WFP kill-switch 或 IPv4/IPv6/DNS 故障注入验收。
- 没有 `validate/prepare → protection active → start/health → commit / 用户显式选择且重验后的 rollback` 的 OS 级事务。
- 没有完整 `BuildModelSnapshot` 或 model revision CAS；final Start gate 不会重新构建/比较已冻结 candidate。
- token 与每代 UUID 已分别承担会话鉴权和进程实例 fence，但都不是持久 runtime owner、配置授权或 Windows OS generation。
- Start/Stop 虽有 process-local 对账协议，仍没有服务端可取消的端到端 deadline；对账自身超时后 handler/barrier 可能继续，GUI 必须保持 indeterminate。Exit 的精确 ACK/finished 闭环也没有把其它命令改成 context-aware executor。
- Go core 仍未独立重复 C++ ConfigBuilder 的全部 Mixed/TUN/系统代理/resolver 产品策略校验。
- Exit ACK 与精确 QProcess finished 只证明同一受管 daemon 的进程内退出；它不证明 Windows TUN、路由、DNS、动态/持久 WFP 或其它 OS 资源已经按预期清理，也不处理 GUI 强杀、崩溃、关机和父进程死亡。

因此现有 TUN reload/退出 guard 必须保留，产品仍为 Alpha/不可发布；不得把本 ADR 用作真实 Windows TUN/WFP、OS 资源退出或无泄漏切线验收证据。

## 验证边界

- `test/runtime_transition_test.cpp` 覆盖原 transition/queue/crash 竞态、daemon 身份快照，以及 `{generation, UUID, PID}` finished tracker 的精确身份、异常退出、重复信号和 finished-before/after-wait 纯状态逻辑。
- Go lifecycle/auth/gRPC 测试覆盖失败 candidate、blocked、旧 reference、命令排序、Exit 的 STOPPED 前置条件/`EXITING` 终态、barrier 先于/后于 Start/Stop/Exit、精确 target/hash/outcome、token + UUID、协议 v2 和真实 localhost gRPC ACK 后 GracefulStop。
- `go/cmd/nekobox_core/internal/boxapi/boxapi_test.go` 覆盖 generation-bound HTTP transport 禁止 keep-alive 复用。
- 完整无 Skip Windows package 会在同轮 GUI 测试程序和刚构建的 package core 上先后运行 tracker 单测与 `core_exit_integration_test`。raw Qt HTTP/2 harness 验证错误 UUID、Exit non-admission 对账 fence、active Exit 拒绝、显式 Stop、结构化 `EXITING` ACK、同一 QProcess `NormalExit/0`，并快照常见 WinINet 五键。

raw harness 不调用产品 `NekoGui_rpc::Client` 或 MainWindow 退出链，不是 GUI→Client→core 端到端测试；它使用无 listener、无 TUN 的最小配置，也不快照适配器、路由、DNS、生产 PID/`2080` 或 WFP。因此仍没有 GUI crash→commit、HTTP/2 timeout/ACK 丢失注入、父进程死亡或真实 Windows TUN/WFP 测试。

## 后果与后续

进程内 Start/Stop、Exit 和 core instance 的直接竞态得到明确 fence，运行状态可附带 generation 与 config hash 供诊断；代价是 blocked、ACK 不确定或进程不退出时会选择持续阻断而不是自动 fallback/kill/replacement。下一步仍按 ADR 0008 建立持久 RuntimeStateMachine、Windows Service、stable anchor 和 persistent WFP，并建立真正 context-aware、可取消的单 owner command executor，再把当前 generation 机制迁移为受保护 OS 事务的一部分。
