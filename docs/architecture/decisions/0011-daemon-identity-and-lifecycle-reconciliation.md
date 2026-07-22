# ADR 0011：daemon 实例身份、生命周期对账与 Exit ACK

状态：已接受（进程内 P0 止损）
日期：2026-07-22

## 背景

GUI 会在同一 localhost 端口、token 和 HTTP/2 client 上重建 `nekobox_core`。本地 QProcess generation 只能描述 GUI 看到的进程，不能证明某次 RPC 实际落在哪一代 daemon。旧实现又把日志中的 `grpc server listening` 当作 ready；该日志实际来自 stderr，监听端口也可能属于旧进程。Start/Stop 响应丢失后，普通状态快照还会与尚未取得 lifecycle mutex 的迟到 handler 竞争，不能安全证明“命令未执行”。

## 决策

1. 每次 `CoreProcess::Start` 前生成新的随机 UUID，并以 `-instance-id` 传给该子进程。UUID 是实例 fence，不是凭据；session token 仍负责鉴权。
2. 每个 GUI RPC 都按值携带 `nekoray_daemon_instance_id`。Start/Stop/对账使用构建候选或 observed runtime 时冻结的精确 UUID，不能在发送时改绑为新的 current daemon。Go interceptor 先验证 token 和 UUID，再允许 handler 读取 command sequence；身份不匹配不得推进 ordering watermark。
3. stdout/stderr 中的 `grpc server listening` 都只触发握手（旧实现只检查 stdout，而 Go `log.Printf` 默认写 stderr）。GUI 必须以该 UUID 调用 `GetDaemonInfo`，核对回显身份和 lifecycle protocol version，成功后才能把同一 `{QProcess generation, UUID}` 标为 ready。旧 core 缺少协议时 fail closed，不提供 legacy fallback。
4. `Restart` 只有在旧 QProcess 的精确 `finished` 已被 `{generation, UUID, PID}` tracker 接受、且 QProcess 已为 `NotRunning` 后才生成并发布 replacement UUID；旧进程仍运行，或仅观察到 `NotRunning` 而缺少匹配 finished 时，都拒绝 kill/terminate 和 replacement。
5. Start/Stop 各自记录 sequence、kind、服务端按收到的精确配置字节计算的 SHA-256 及终态 outcome。`ReconcileLifecycle` 自身取得更高 command sequence，并与 Start/Stop/Exit 共用同一 lifecycle mutex：
   - 目标命令先取得锁时，对账等待它完成并返回终态；
   - 对账先取得锁时，先推进 watermark，迟到的低序号目标随后必被拒绝。
6. 对账返回 daemon UUID、ordering watermark、目标命令记录、core phase、process-local runtime generation、active Start sequence 与当前 config SHA。重复对账不覆盖最后一条 lifecycle 命令记录。
7. GUI 只接受以下确定结论：精确 Start 记录为 succeeded 且 active sequence/config hash 一致；Start 为 failed-clean 或已被屏障挡住且 daemon stopped；Stop succeeded 或已被屏障挡住且 daemon stopped；Stop 未被执行且同一精确 config 仍 active。其它状态、未知枚举、hash/identity/sequence 不一致或对账再次超时一律保持 indeterminate/fail-closed。
8. lifecycle protocol v2 将 Exit 响应改为结构化 lifecycle snapshot。Go 仅在精确 `STOPPED` 时原子提交 `EXITING`，ACK 必须回显同一 daemon UUID、command sequence/watermark、空 current/config、`EXIT/SUCCEEDED`；active/blocked 不会被隐式 Stop。响应形成后通过 handler 外 one-shot callback 调用 gRPC `GracefulStop`，避免同步停止当前 handler 的死锁。
9. GUI 在发送 Exit 前冻结 `{QProcess generation, daemon UUID, PID}`。精确 ACK 后持续等待该 QProcess 的 `finished` 且必须为 `NormalExit/0`；tracker 支持 finished-before-wait 与 wait-before-finished。等待期间不 kill/replace、不恢复 UI/保存 fence，重复退出请求也不能绕过 continuation fence。
10. Exit ACK 丢失时，以更高 sequence 对账同一 target Exit。只有 `STOPPED + FENCED_NOT_ADMITTED + no current + empty hash` 能证明低序号 Exit 未接纳且已被封住，允许取消本次 GUI 退出；其它结果（包括 server 已因 `EXITING` 进入 GracefulStop 而无法对账）都必须继续等待同一 PID finished，不能把 transport failure 当成安全重试/恢复依据。

## 明确边界

- `ACTIVE` 只证明该 daemon 发布了 candidate，不证明线路健康、Mixed 可达、TUN/WFP/路由无泄漏。
- `STOPPED` 只证明该 daemon 没有保留 `managedCore`；凡发生 cleanup 且结果不能确认都会进入 `BLOCKED`。它不是 Windows OS 资源事实源。
- `BLOCKED` 表示清理结果不确定，不能称为 active 或 stopped。
- config SHA 只标识 core 收到的精确配置字节，不等于磁盘/model revision 或语义等价。
- Go runtime generation 仅在一个 daemon UUID 内有意义，不能与 GUI transition generation 或 QProcess generation 比较。
- 同 mutex 的对账只能返回稳定终态或超时 unknown，不是 STARTING/STOPPING 实时进度 API。
- client abort 仍不会取消正在运行的 Go Start/Stop。对账本身超时后，服务端屏障可能稍后生效，但 GUI 仍必须保留 unknown。
- Exit ACK/finished 只在当前 GUI-owned daemon 内闭环；ACK 不确定且无法证明未接纳时，GUI 会无限保持退出 fence 并按间隔提示。这是有意的 fail-closed continuation，不是通用 deadline/cancellation 机制。
- `GracefulStop` 会等待其它已在途 RPC；现实现没有统一 context-aware command executor，长时间不返回的 handler 仍可能使进程长期停在 `EXITING`。
- 没有持久 Runtime Service、stable anchor、WFP、desired/observed/owner/health 状态机或 Windows OS 事实源，产品仍为 Alpha/不可发布。

## 验证

- Go lifecycle 测试覆盖屏障先于迟到 Start/Exit、屏障等待 Start/Stop、成功、failed-clean、Close/cleanup blocked、精确 target/hash/outcome、`EXITING` 终态和 stale sequence；真实 localhost gRPC 测试验证结构化 ACK 可交付后 Serve 才由 `GracefulStop` 结束。
- auth 测试覆盖 token + UUID、缺失/错误/重复 UUID，以及鉴权后清除敏感 metadata、保留已验证身份。
- gRPC 测试覆盖握手回显、协议版本 2、Start/Stop/Exit 对账响应和非递增屏障拒绝。
- C++ 状态测试覆盖 `{generation, UUID}` 原子快照、握手结果的精确身份接受逻辑、原有 queue/crash generation 竞态和 `{generation, UUID, PID}` finished tracker。
- 完整无 Skip Windows package 另用刚构建的 package core 运行 raw Qt HTTP/2/QProcess harness，验证错误身份、Exit non-admission 对账 fence及迟到低序号 Exit 拒绝、active Exit 拒绝、显式 Stop、精确 `EXITING` ACK、同 PID `NormalExit/0` 及常见 WinINet 五键前后不变。

该 raw harness 不执行产品 `Client::Exit` 和 MainWindow continuation 流程；其 core 配置无 listener、无 TUN，WinINet 五键快照也不是适配器、路由、DNS、生产 PID/`2080` 或 WFP 事实源。GUI→Client 端到端、HTTP/2 ACK 丢失注入、父进程死亡、Windows TUN/WFP 和 Mixed 线路切换仍需独立集成验收。
