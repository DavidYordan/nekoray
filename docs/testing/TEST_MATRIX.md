# 测试矩阵

状态：现行
最后更新：2026-07-22

## 证据要求

每次发布候选至少记录：提交号、GUI/core 文件哈希、Windows 版本、命令、预期结果、实际结果和脱敏报告路径。日志、导出配置和报告可能含节点名、服务器地址、路由、进程及本地路径，不得提交真实凭据。

Windows 是唯一产品平台。OpenWrt 是相同 core 的诊断环境，不是产品兼容目标，也不能代替 Windows 发布验收。

历史一次性 Mixed/AnyTLS 调查只在矩阵中作为明确标注日期的诊断背景保留；原始证据见 [2026-07-20 接管基线](../archive/audits/2026-07-20-takeover-baseline.md)。历史中的单次成功不能升级为当前通过结论。

## 2026-07-22 无侵入回归

| 项目 | 结果 | 证明范围 |
|---|---:|---|
| 当前源码 Windows 全量 C++/package 本地重编译 | 成功 | 包含本批 group speedtest/UI transition 改动并成功链接；只证明本构建树可编译，不是 release/deployment |
| 两个 Go 模块普通测试 | 通过 | core 包含 nil-config/system-fallback/FullTest 与本批 lifecycle/generation 回归；`grpc_server` 普通测试通过 |
| Go 重复/race/vet | 通过 | core 的 `go test -count=20 ./...`、`go test -race ./...`、`go vet ./...` 通过；`grpc_server` 普通、race、vet 通过。只验证进程内并发断言，不是 GUI/父进程/Windows TUN/WFP 生命周期测试 |
| `test_final_config_guards.ps1` | 10/10 | 隔离导出路径的部分最终配置 guard；不等于 live/test TUN 四象限、export 删除或全部 dialer-bearing 路径的 C++ golden |
| `test_config_preservation.ps1` | 10/10 | 损坏主/路由、错误辅助映射、非法活动路由路径、未知 profile 与悬空 group 引用均保持原件不变并生成可验证隔离证据；显式恢复报告/回滚通过，未完成事务在配置加载前阻断且原件不变 |
| `test_verify_mixed_openwrt.py` | 19/19 | 远端 helper 收紧与命令安全单测；未执行真实远端协议重测 |
| `test_mixed_probe.ps1` | 7/7 | loopback Mixed、拒绝项、额外 listener/系统代理/日志/origin 清理 |
| `test_runtime_connectivity.ps1` | 正例通过、反例正确拒绝 | expected 204 时 HTTP/SOCKS5h 均 204；expected 200 时报告 2 项 mismatch；系统代理、端口与 origin 清理通过 |
| CTest | 2/2 | 在项目 MinGW `bin` 已加入 `PATH` 的环境中通过。`runtime_transition` 覆盖 transition 单一 owner、跨线程完成、迟到 completion 不得释放新代/depth gate、失败获取保留 active/pending gate、pending crash cleanup 连续 handoff/重复 crash 链，以及 daemon/profile-request generation、ready 后 queue 的 immediate one-shot 顺序行为、MarkReady/Queue 真并发恰好一次、旧 crash timer、ready event 单次消费、显式取消、新请求覆盖、跨 daemon 失效与 cancel/consume 竞争。Go lifecycle 测试另覆盖较新 Stop 拒绝迟到旧 Start，以及缺失/零 Exit command sequence 拒绝。没有 QProcess/GUI/真实 HTTP/2 超时集成；这些测试也不代表订阅/非空 group、完整 ConfigBuilder snapshot 或 Windows TUN/WFP 已验收 |
| Windows quality CI | 通过 | 仓库卫生、固定子模块、受控 core 源构建、Go 普通测试和 verifier 安全契约；不覆盖 GUI/TUN/WFP |

本轮测试使用重建后的 `build-package-windows64/nekobox.exe`（SHA-256 `FDD4901C2836FACBCAAC6271B0D3463F8AB064458DD40BFACAB07C2B3F16BD15`）和 `nekobox_core.exe`（SHA-256 `6FE7236AF6866E6789CD41F3A0DA5C1F88ACCEC209E328E5449E4543AD0CE7A1`）；`deployment/windows64/` 仍是旧产物。这些 hash 只是本地审计快照，不是 release manifest。以上均不是 Windows TUN/WFP、系统代理 broker、GUI 退出或线路切换验收。

## 三级验证矩阵

| 级别 | 范围 | 必测项目 | 当前结论 | 发布要求 |
|---|---|---|---|---|
| L1 本地无侵入 | 配置/schema | 每个导出配置执行 `nekobox_core check`；空配置、迁移、损坏配置 | 已验证损坏主/路由配置及错误类型、非字符串、重复辅助映射原件不被覆盖；其它迁移矩阵不完整 | 必须自动化通过 |
| L1 本地无侵入 | Mixed contract | HTTP absolute-form、HTTPS CONNECT、SOCKS5h、认证正反例、端口占用、非 loopback/TUN 拒绝 | 正向及安全收紧有证据，反例不完整 | 必须全部通过 |
| L1 本地无侵入 | 端口映射/OS 副作用 | 主 `12080` 命中当前主 profile；每个辅助端口命中其绑定 profile；顶层 custom 不得改变完整受管 listener/outbound 快照；未授权/非精确 TUN、`set_system_proxy=true` 及已知系统 endpoint/时钟副作用必须拒绝；live/test 的 TUN on/off 四象限应等于上游 `dataStore->spmode_vpn`，export 应删除接口自动检测字段 | 生成表达式与 Managed TUN validator 已审计；NTP/嵌套 route direct-action bind 覆盖已纳入 guard，三份 loopback fixture 无必要 `true` 已移除。导出 fixture 仅覆盖部分 OS guard，上述四象限/export 边界和 Mixed/TUN 完整 C++ golden 仍缺 | 必须通过 |
| L1 本地无侵入 | 工具安全 | 不改系统代理/TUN/路由/DNS；拒绝 TUN、系统 NTP 写入和非空 endpoints；只保留目标 outbound detour 闭包并只结束精确 PID；不触碰 `D:\Program Files\nekoray` 和 `2080` | 本地/远端收紧器已有 fixture；OpenWrt Python 单测 19/19 | 必须保持 |
| L2 OpenWrt 探针 | core/工具安全 | 相同 `1.13.12-routefluent-anytls-client.7` core 的 schema、loopback Mixed、监听 PID 与远端基线保护 | 2026-07-20 历史执行：既有 PID/命令行、配置/manifest 哈希和监听均不变，临时目录已清理；旧探针对临时副本强制 `auto_detect_interface=true`，尚未按默认 preserve 重跑 | 必须重跑并保持 |
| L2 OpenWrt 探针 | 远端链路 | Trojan、AnyTLS、DNS、detour 有/无的对照；HTTP/CONNECT/SOCKS5h | 2026-07-20 历史诊断：AnyTLS mihomo 无 detour 与独立 profile 2 Trojan 均三协议 204；主 AnyTLS + `g-2` detour 失败。因所有变体均被旧探针强制 `auto_detect_interface=true`，只支持同一变体内的组合归因，不能作为当前导出策略验收 | 主组合阻断发布，按 preserve 重跑 |
| L1 本地无侵入 | 遥测一致性 | worker 更新 counter/rate 时 UI/JsonStore 只能读取同一代不可变快照，或由统一锁/原子协议保护；Reset 与持久化也必须纳入 | `last_update=0` 已消除未初始化读取；`TrafficBinding` 只隔离 profile/tag 身份。共享 `TrafficData` 的 counter/rate 仍存在无锁跨线程访问，尚无并发测试 | P2，稳定版前关闭 |
| L3 Windows 集成 | 生命周期 | GUI 退出/重启、线路重启、core 崩溃与显式回滚；RPC expected generation/端到端 deadline/indeterminate 对账 | 进程内 transition/lifecycle 单元基础、atomic `core_running`/`prepare_exit`、`QSemaphore` 完成通知已落地；session-local command sequence 阻止同一 daemon 内旧 Start 反超新 Stop，退出链只在收到响应 daemon 已接受且按序完成的 Stop 后继续，失败/不确定 Stop 不调用 Exit/quit。该响应不证明 daemon generation。Start/Stop 仅有 30 秒 client abort，超时仍 indeterminate，Go handler 不保证中止；RPC 仍不携带 expected daemon generation、无状态查询/跨 daemon 对账，且无 QProcess/GUI/真实 HTTP/2 超时集成验收 | 阻断发布 |
| L3 Windows 集成 | 网络控制 | 系统代理、Wintun、persistent WFP kill-switch、IPv4/IPv6、DNS、PID/资源所有权 | 内部 TUN 活动配置已强制 strict+IPv4/IPv6，但 WFP 仍随 worker 消失；系统代理 UI 暂禁，精准 broker 未实现 | 阻断发布 |
| L3 Windows 集成 | GUI/安装更新 | clean build、干净用户目录、安装/更新失败与回滚 | 本轮本地 GUI/core 目标重编译有证据；干净环境、完整打包、安装/更新与回滚矩阵未完成 | 阻断稳定版 |
| L1/L3 | C++/Go/脚本自动测试 | CTest、自有 Go 模块与隔离导出 fixture | CTest 有恢复/事务基础设施，并新增 process-local transition/daemon generation 单元测试；queue-or-ready 已覆盖顺序与 MarkReady/Queue 真并发恰好一次，尚无 QProcess/GUI crash→commit、真实 HTTP/2 timeout、TrafficData 并发或 ProfileManager/订阅完整 harness。两个 Go 模块普通测试通过，core module 的重复/race/vet 通过；PowerShell/Python fixture 有独立证据，但 ConfigBuilder/import 和持久 runtime 仍缺 C++/Windows 集成矩阵 | 阻断稳定版 |

## 逻辑线路与接口选择的断言

- 默认 Mixed 端口固定为 `12080`；`2080` 是外部生产实例，不是兼容或迁移目标。
- 主/辅助 Mixed 入口必须强绑定各自逻辑 profile/outbound。标准路由规则不得将其改送 `direct`、`block` 或另一条线路；最终 custom 合并若破坏绑定必须在启动前失败关闭。
- 断言对象不仅是 tag/port/detour：合并前捕获的完整受管 listener 和全部可达 outbound 必须在合并后逐项一致，且精确 terminal binding 必须先于可能改投/提前 resolve 的规则。这个断言不自动覆盖未受管的所有路由/DNS 语义。
- `auto_detect_interface` 仅是底层 OS 接口选择，不能改变入口到逻辑线路的绑定，也不能作为“Mixed 自动选线路”的验收依据。
- 本机生产 TUN 存在时，本地远端成功只能证明请求完成，不能单独证明物理出站归属。此时按 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md) 做 L2 对照。

L2 历史诊断细节见[已验证基线](OPENWRT_REMOTE_LAB.md#已验证基线)。2026-07-20 的旧探针对每个临时变体强制 `auto_detect_interface=true`；它只能在这个共同条件下把故障收敛到 AnyTLS mihomo client 与 `g-2` detour 的组合，不能证明产品导出接口策略。必须按当前默认 preserve 重跑后才能成为现行证据，更不证明 Windows Wintun、WFP、系统代理、GUI 或线路重启已通过。

## 发布前最小矩阵

1. 在干净的 Windows 用户目录启动，不复用唯一真实配置，不接触生产 Nekoray。
2. 覆盖空配置、旧配置迁移、损坏配置和恢复流程。
3. 完成 Mixed 三种协议、认证正反例、端口占用、非法端口与主/辅端口映射。
4. 完成 Trojan、AnyTLS 及有/无 front proxy；L2 只能补充远端归因，最终仍需 L3 Windows 证据。
5. 验证只有用户手动操作才能启停系统代理/TUN；GUI 退出、重启和线路切换不改变它们。
6. 分别核对 TUN requested、worker-observed 和 Windows 实际状态；core 崩溃后当前只恢复空控制 core的行为必须被识别为未通过，而不是把 requested 误判为 TUN 已恢复。
7. 在保护持续生效时完成线路重启和故障注入，证明不会 direct 泄漏；提交后故障必须进入阻断，不得自动切回旧线路。只有用户明确选择、旧 generation 重新验证后才测试显式回滚。
8. 覆盖 worker 无 instance、worker kill、service/BFE restart，确认 TCP、UDP、HTTP helper 和物理 IPv4/IPv6/DNS 均无系统 fallback。
9. 覆盖空订阅、HTML 错页、超时、解析失败和正常更新；失败时旧组原样保留。
10. 覆盖更新失败、校验失败和回滚；修复更新器前不得进行真实更新测试。

## 工具入口

- `tools/export_profile_core_config.ps1`：导出单个 profile 的实际 core 配置并可执行 `check`。
- `tools/verify_mixed_inbound.ps1`：只接受主 `mixed-in -> proxy` 连通性诊断；拒绝 TUN/系统 NTP 写入/endpoints、裁剪到 `proxy` 的精确 detour 闭包后启动临时配置，验证监听 PID 和三种代理请求；不是辅助映射 contract 或 Windows 集成验收器。
- `tools/verify_mixed_openwrt.py`：在固定 `192.168.1.7`、`127.0.0.1:52080` 和唯一临时目录中执行相同的严格收紧与 L2 探针；必须先 `--dry-run`，真实结果须满足远端基线未变和清理完成。
- `test/fixtures/mixed-direct-sanitization.json`：验证诊断脚本不会启动额外 LAN inbound/controller、修改系统代理或写配置指定日志。
- `test/test_mixed_probe.ps1`：用 loopback HTTP 204 origin 运行 direct、dummy auth 与安全拒绝 fixtures，并核对端口、系统代理、文件副作用和 origin 清理；不依赖公共站。2026-07-20 为 7/7。
- `test/test_verify_mixed_openwrt.py`：覆盖远端收紧器的接口字段、NTP/endpoints、精确 detour 闭包、敏感摘要与命令安全；当前 19/19。
- `test/test_runtime_connectivity.ps1`：在临时 package 和 loopback HTTP 204 origin 中验证运行快照脚本的 PID 归属、精确 HTTP 状态、错误期望拒绝及清理；2026-07-20 的 204 正例通过，错误期望 200 正确失败。
- `test/config_recovery_test.cpp`：覆盖单/多文件恢复基础设施、`VerifiedBefore`/`VerifiedAfter`/`Indeterminate`、退役、hidden/unexpected/exact-case/staging，以及 terminal startup/report 分层校验。路径用例 `routes_box/ROUTE~1` 只证明 `~` 被词法规则拒绝，不表示测试构造了真实 Windows 8.3 alias；选定配置根本身的 junction/别名仍需操作者确认。
- `test/runtime_transition_test.cpp`：覆盖 GUI process-local transition 单一 owner、worker 完成当前 ticket、迟到 completion 不得释放新 generation/清除新代 depth gate、失败获取保留 active/pending gate、pending crash cleanup 连续 handoff与重复 crash 链，以及 daemon/profile-request generation、ready 后 queue 的 immediate one-shot 顺序行为、MarkReady/Queue 真并发恰好一次、旧 crash timer、ready event 单次消费、显式取消、新请求覆盖旧事件、跨 daemon 失效和 cancel/consume 竞争；仍不创建 QProcess、GUI、core 进程，不触发真实 HTTP/2 30 秒 abort，也不覆盖 `TrafficData` 并发或 Windows TUN/WFP。
- `go/cmd/nekobox_core/core_lifecycle_test.go`：覆盖失败 candidate 不发布、清理/Close 失败进入 blocked、旧/stopped reference 与 HTTP client 不得跨代、dial/stats 与 Stop 互斥、并发 Start 只发布一次、active/blocked lifecycle 拒绝 Exit。
- `go/cmd/nekobox_core/internal/boxapi/boxapi_test.go`：除无 instance fail-closed 外，覆盖 generation-bound HTTP transport 禁用 keep-alive，防止连接跨代复用。
- `test/test_config_preservation.ps1`：在隔离 appdata 中启动配置导出路径，验证已存在但损坏的主/路由配置、重复/非字符串/错误 JSON 类型的辅助映射、非法活动路由路径、未知 profile 类型与悬空 group 引用保持 SHA-256 不变，并验证 snapshot/metadata；同时验证显式事务报告与 before 回滚，以及 pending 事务在加载前阻断且主配置不变，当前为 10/10。
- `test/test_final_config_guards.ps1`：在隔离临时 appdata 中验证安全 `internal-full` 文件导出、部分 TUN/系统代理/system endpoint/NTP 副作用拒绝，以及标准 SOCKS profile 的测试态 custom、Managed Mixed listener、detour 禁改和 custom route 空字段回归；2026-07-20 为 10/10。不启动线路，也不覆盖 live/test 的 TUN on/off 四象限、export 删除字段或所有 NTP/direct-action dialer，不能代表完整 C++ golden。
- `tools/verify_runtime_connectivity.ps1`：采集整套运行时快照；不是自动验收器。
- `tools/verify_fail_closed_restart.ps1`：采集 fail-closed 相关状态；使用限制见 [fail-closed 验证](FAIL_CLOSED.md)。

这些工具调用的 `nekobox_core check/run` 是[显式高级 CLI](../reference/CLI.md#core-高级-cli)，会直接读取经过导出或收紧的临时配置；它们不走普通 GUI，也不能用于证明 Go 层已经独立实施产品策略。
