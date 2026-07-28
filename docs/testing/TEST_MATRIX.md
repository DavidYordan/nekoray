# 测试矩阵

状态：现行证据台账；产品断言服从 [产品方向与开发契约](../PRODUCT.md)
最后更新：2026-07-28

## 证据要求

每次发布候选至少记录：提交号、GUI/core 文件哈希、Windows 版本、命令、预期结果、实际结果和脱敏报告路径。日志、导出配置和报告可能含节点名、服务器地址、路由、进程及本地路径，不得提交真实凭据。

Windows x64 是当前首要验收平台。OpenWrt 是相同 core 的诊断环境，不是产品兼容目标，也不能代替 Windows 验收。

历史一次性 Mixed/AnyTLS 调查只在矩阵中作为明确标注日期的诊断背景保留；原始证据见 [2026-07-20 接管基线](../archive/audits/2026-07-20-takeover-baseline.md)。历史中的单次成功不能升级为当前通过结论。

## 2026-07-28 主入口与专用端口路由整改

| 检查 | 结果 | 边界 |
|---|---:|---|
| 修复前定向回归 | 14/15 | 新增 `primary_mixed_preserves_native_routing` 后，旧构建只有该用例失败，证明断言命中了既存的无条件主线路绑定 |
| 增量重建 `nekobox` | 通过 | 重新编译 `ConfigBuilder.cpp` 并链接 GUI，不等同于完整 Windows 打包 |
| `test_final_config_guards.ps1` | 15/15 | 原生 `mixed-in` 保留自定义 route 命中，不再存在仅凭 `mixed-in` 无条件终结到 `proxy` 的规则；该安全导出路径不包含运行态辅助端口 |
| 辅助端口 reject 编译红绿回归 | 通过 | 旧实现只产生 terminal binding，新 `auxiliary_route_compiler_test` 要求复制三个显式 reject 且不复制 resolve/direct/bypass/其它 outbound；修复前失败、修复后通过 |
| 辅助端口 core schema fixture | 通过 | `test/fixtures/auxiliary-reject-routing.json` 使用文档保留地址，`nekobox_core check -c` 返回 0；不启动 listener 或远端连接 |
| CTest | 5/5 | 配置恢复、runtime transition、分享格式、辅助端口路由编译和 resolver policy 纯测试通过 |

本轮没有启动真实 GUI、线路或 Windows TUN，也没有执行完整 package。主入口回归与辅助端口 reject/terminal 顺序已有配置级红绿证据；真实双线路出口、单线失败隔离和 Windows 集成仍待验证。

## 2026-07-24 端口恢复与 Clash TUN 归因

| 项目 | 结果 | 证明范围 |
|---|---:|---|
| Windows 完整无 Skip 打包 | 通过 | clean reset 并重建 GUI/core；raw core Exit gate PASS；私人配置 219 个文件在打包后恢复 |
| CTest | 4/4 | 配置恢复、runtime transition、分享格式和 resolver policy 纯测试 |
| `test_final_config_guards.ps1` | 15/15 | 默认主 Mixed 断言为 `2080`；原生 WD 与 provider DoH 两类 resolver 路径及 OS 副作用拒绝均通过 |
| `test_mixed_probe.ps1` | 7/7 | HTTP absolute-form、CONNECT、SOCKS5h 进入受控回环目标；额外 listener、系统代理、日志和 origin 状态不变或已释放 |
| `test_runtime_connectivity.ps1` | 正例通过、反例正确拒绝 | HTTP/SOCKS5h 204 正例及错误期望状态反例；不证明真实节点可达 |
| 仓库卫生 | 通过 | 401 个 tracked 文件、12 个 PowerShell、43 个 Markdown、112 个本地链接；零失败 |
| 当前 WD profile 0 导出与 `check` | 通过 | 生成 `127.0.0.1:2080` Mixed，0 个 provider resolver group；证明配置可解析，不证明远端线路健康 |
| 当前真实节点经 Clash TUN | 失败并完成分层归因 | Mixed 三协议均命中 `proxy` 后在 Trojan TLS/AnyTLS session 层失败；进程级物理接口+真实 IP 的一次性 TLS 对照成功，说明 Clash global/Fake-IP 路径会改变结果 |
| OpenWrt 对照 | 未执行 | `192.168.1.7` 无 ARP，SSH 未取得 banner；不能据此判定线路失败 |

本轮最终本地审计快照：`nekobox.exe` SHA-256 `888CBADFB6D308F738D718F32D0789646B58155FA827C19D5DEE256E03AB5F3C`；`nekobox_core.exe` SHA-256 `2FEE446DD5AB0C0A3B374F826D77C21B9145C263B11F05F27282DB5575AD4ED3`；zip SHA-256 `7C37A9E3A52ADE3EA7D97E4AD566E65370E5DB93F8678CE858EC73474F62F7F3`；RouteFluent manifest SHA-256 `06CF3AAF4E2DF52311D7B61FA9D34C44F382BDC654AB4B1DBECAC347B8C724E5`。这些文件位于被忽略的本地 deployment，不是正式 release manifest。

当前私人配置主端口为 `2080`，辅助池仍为 `12100..12299`，既有绑定仍为 `89:12100`、`113:12101`。迁移前副本保存在 deployment 的 recovery 目录，不提交 Git。测试期间没有停止或改写 Clash TUN，也没有启停系统代理或项目 TUN。

## 2026-07-22 无侵入回归

| 项目 | 结果 | 证明范围 |
|---|---:|---|
| 当前源码 Windows 全量 C++/package 本地重编译 | 成功 | 无 Skip 流程先以受保护 helper 清空并重建 GUI build tree，再编译 lifecycle v3、QProcess finished tracker、分享格式测试和 raw integration harness；没有继承旧 CMake cache、object 或手工诊断 core。只证明本机工具链的 clean provenance，不是独立 clean-room release |
| 两个 Go 模块普通测试 | 通过 | core 包含 nil-config/system-fallback/FullTest、lifecycle generation、deadline 准入 fence、Start 取消/发布、Exit `EXITING`/对账与真实 localhost gRPC ACK→GracefulStop；`grpc_server` 覆盖 token + UUID、协议版本 3、one-shot shutdown controller 与 metadata 边界，不是 GUI/Windows 资源集成测试 |
| Go 重复/race/vet | 通过 | core 的 `go test -count=20 ./...`、`go test -race ./...`、`go vet ./...` 通过；`grpc_server` 普通、race、vet 通过。只验证进程内并发断言，不是 GUI/父进程/Windows TUN/WFP 生命周期测试 |
| `test_final_config_guards.ps1` | 15/15 | 隔离导出覆盖原生域名无 provider DoH、有效/过期订阅组 resolver 元数据、域名 DoH 经 `dns-local` bootstrap、非法 DoH 拒绝、bootstrap custom 替换拒绝及既有 OS/路由 guard；不等于 live/test TUN 四象限、完整订阅导入或断网 DNS 泄漏观测 |
| `test_config_preservation.ps1` | 10/10 | 损坏主/路由、错误辅助映射、非法活动路由路径、未知 profile 与悬空 group 引用均保持原件不变并生成可验证隔离证据；显式恢复报告/回滚通过，未完成事务在配置加载前阻断且原件不变 |
| `test_verify_mixed_openwrt.py` | 19/19 | 远端 helper 收紧与命令安全单测；未执行真实远端协议重测 |
| `test_mixed_probe.ps1` | 7/7 | loopback Mixed、拒绝项、额外 listener/系统代理/日志/origin 清理 |
| `test_runtime_connectivity.ps1` | 正例通过、反例正确拒绝 | expected 204 时 HTTP/SOCKS5h 均 204；expected 200 时报告 2 项 mismatch；系统代理、端口与 origin 清理通过 |
| 批量分享格式 | C++ 实现/纯测试通过 | 右键多选保留含 remark 原生链接；新增无 fragment 链接与严格 `ip:port:user:pass`。纯函数覆盖字面 IPv4 SOCKS5/非 TLS HTTP 正例，及域名/IPv6/其它协议/TLS/缺凭据/非法端口/冒号换行负例，只用假凭据；GUI 代码为全有或全无，但真实剪贴板不变仍缺 GUI 自动化 |
| CTest | 4/4 | 在项目 MinGW `bin` 已加入 `PATH` 的环境中通过：配置恢复、runtime transition/finished tracker、分享格式纯函数，以及 WD/NEX resolver 来源、DoH URL/bootstrap/strict group 纯测试。CTest 不创建 QProcess/GUI/core，也不操作系统剪贴板或执行真实 HTTP/2；这些纯测试不代表完整 ProfileManager 刷新、真实 DNS 网络行为或 Windows TUN/WFP 已验收 |
| raw core Exit integration | 完整无 Skip package gate PASS | 用随机 loopback control port、无 listener/无 TUN 配置和刚构建 core，验证 lifecycle v3 握手/deadline、错误 UUID 为 gRPC 16、Exit non-admission 对账 fence/迟到命令拒绝、active Exit 为 gRPC 9、显式 Stop、结构化 `EXITING` ACK、同一 QProcess `NormalExit/0` 和常见 WinINet 五键不变。它不调用产品 `Client::Exit`/MainWindow，不验证生产 PID/`2080`、适配器、路由、DNS、TUN 或 WFP |
| Windows quality CI | 通过 | 仓库卫生、固定子模块、受控 core 源构建、Go 普通测试和 verifier 安全契约；不覆盖 GUI/TUN/WFP |

本轮测试使用受保护 clean reset 后重建的 GUI 和 package core：`build-package-windows64/nekobox.exe` 与 `deployment/windows64/nekobox.exe` 的 SHA-256 均为 `3E918885EBB20D0A00FF04FD43E16841E5C0453CCD324C6F5EDE2BB3C3EBB43D`；core 仅存在于 `deployment/windows64/nekobox_core.exe`，SHA-256 为 `F545DC44627B83DAF49786F3403ED9E464783D71E6917CE06FDFFC0E147D09E5`，不声称 build tree 中存在第二份 core。不带 Skip 参数的完整打包及 tracker/share/resolver policy/raw Exit gate 已成功，zip SHA-256 为 `86F3CD775DFF03B13FF6A66DC225FFA1BDDA0B919D504542384C0D743CFBC306`，package RouteFluent manifest SHA-256 为 `28100CC9F77DE340A3B76A873E476B8EA9D4ECB115B1BA347FFF57345184760A`。215 个 package 配置文件已恢复，没有 preserve 或手工诊断产物残留。这些 hash 只是被忽略的本地审计快照，不是 release manifest。以上均不是 Windows TUN/WFP、系统代理 broker、GUI→Client 退出或线路切换验收。

历史的 2026-07-22 打包/Exit gate 前后快照记录了当时外部 NekoRay 占用 `2080`；该环境已被 2026-07-24 的 Clash TUN 基础网络替代，只保留为历史证据，不再约束当前产品端口。

## 三级验证矩阵

| 级别 | 范围 | 必测项目 | 当前结论 | 发布要求 |
|---|---|---|---|---|
| L1 本地无侵入 | 配置/schema | 每个导出配置执行 `nekobox_core check`；空配置、迁移、损坏配置 | 已验证损坏主/路由配置及错误类型、非字符串、重复辅助映射原件不被覆盖；其它迁移矩阵不完整 | 必须自动化通过 |
| L1 本地无侵入 | Mixed contract | HTTP absolute-form、HTTPS CONNECT、SOCKS5h、认证正反例、端口占用、非 loopback/TUN 拒绝 | 正向及安全收紧有证据，反例不完整 | 必须全部通过 |
| L1 本地无侵入 | 端口映射/OS 副作用 | 主 `2080` 保持上游路由语义；每个专用端口命中其绑定完整 chain；显式 reject/block 可生效；顶层 custom 不得改变专用 listener/outbound 绑定；无明确操作不得改变系统代理/TUN | 2026-07-28 已用导出红绿回归关闭主入口无条件绑定；辅助 reject/terminal 编译已有纯 C++ golden 和 core schema fixture，仍缺完整 ConfigBuilder 运行态 golden 与真实双线路验证 | 必须通过 |
| L1 本地无侵入 | 工具安全 | 不改系统代理/TUN/路由/DNS；拒绝 TUN、系统 NTP 写入和非空 endpoints；只保留目标 outbound detour 闭包并只结束精确 PID；不停止或改写 Clash TUN | 启动 GUI/core、写审计报告及构建/临时目录的脚本参数继续使用固定磁盘、非生产/非 reparse 路径护栏；本地/远端收紧器已有 fixture，OpenWrt Python 单测 19/19 | 必须保持 |
| L2 OpenWrt 探针 | core/工具安全 | 相同 `1.13.12-routefluent-anytls-client.7` core 的 schema、loopback Mixed、监听 PID 与远端基线保护 | 2026-07-20 历史执行：既有 PID/命令行、配置/manifest 哈希和监听均不变，临时目录已清理；旧探针对临时副本强制 `auto_detect_interface=true`，尚未按默认 preserve 重跑 | 必须重跑并保持 |
| L2 OpenWrt 探针 | 远端链路 | Trojan、AnyTLS、DNS、detour 有/无的对照；HTTP/CONNECT/SOCKS5h | 2026-07-20 历史诊断：AnyTLS mihomo 无 detour 与独立 profile 2 Trojan 均三协议 204；主 AnyTLS + `g-2` detour 失败。因所有变体均被旧探针强制 `auto_detect_interface=true`，只支持同一变体内的组合归因，不能作为当前导出策略验收 | 主组合阻断发布，按 preserve 重跑 |
| L1 本地无侵入 | 遥测一致性 | worker 更新 counter/rate 时 UI/JsonStore 只能读取同一代不可变快照，或由统一锁/原子协议保护；Reset 与持久化也必须纳入 | `last_update=0` 已消除未初始化读取；`TrafficBinding` 只隔离 profile/tag 身份。共享 `TrafficData` 的 counter/rate 仍存在无锁跨线程访问，尚无并发测试 | P2，稳定版前关闭 |
| L3 Windows 集成 | 生命周期 | GUI 退出/重启、线路重启、core 崩溃；与 4.0.1 对照无额外限制或数据丢失 | 当前已有 UUID/executor/对账改造，但缺完整 GUI 证据，且复杂度可能超过需求 | 按上游回归审计 |
| L3 Windows 集成 | 网络控制 | 系统代理、TUN、IPv4/IPv6、DNS 的手工操作与上游回归 | 当前加入了多项额外 guard；persistent WFP 不属于现行核心要求 | 不得比上游退化 |
| L3 Windows 集成 | GUI/安装更新 | clean build、干净用户目录、安装/更新失败与回滚 | 无 Skip package 先 clean reset GUI build tree、强制 `BUILD_TESTING=ON`，在同轮 GUI tests 与刚构建 core 上依次运行 tracker 和 raw Exit，只有通过才写正式 zip；任一 Skip 只产诊断目录且不创建/覆盖 zip。独立 clean-room 环境、安装/更新失败与回滚矩阵仍未完成 | 阻断稳定版 |
| L1/L3 | C++/Go/脚本自动测试 | CTest、自有 Go 模块与隔离导出 fixture | CTest 为 4 项纯测试；tracker/分享格式/resolver policy 和 Go 单测覆盖协议 v3 deadline、Start 取消/发布、Exit/对账/finished 顺序，完整 package 另有真实 core raw QProcess/HTTP2 gate。尚无 GUI→Client crash→commit、真实 timeout/ACK 丢失、分享剪贴板、TrafficData 并发或 ProfileManager/订阅完整 harness。两个 Go 模块普通测试通过，core module 的重复/race/vet 通过；PowerShell/Python fixture 有独立证据，但完整 ConfigBuilder/import、DNS 泄漏观测和持久 runtime 仍缺 C++/Windows 集成矩阵 | 阻断稳定版 |

共享路径护栏只是 fail-closed 的路径前置筛选：它没有通过最终文件句柄验证 final-file identity，不能声称已解决所有 hardlink 或其它同文件别名。报告/导出工具 `export_profile_core_config.ps1`、`verify_runtime_connectivity.ps1` 和 `verify_fail_closed_restart.ps1` 会拒绝覆盖已存在的目标文件；此项必须与路径护栏分开测试，且不得当作完整文件身份保证。

## 逻辑线路与接口选择的断言

- 默认 Mixed 端口保持上游值 `2080`；专用端口范围是可调整的实现参数。
- 主 `2080` 必须保留上游路由/reject/DNS 语义；专用线路入口才强绑定完整 profile chain，并拒绝改投 `direct`、主线或另一条线路。
- 专用入口的断言对象不仅是 tag/port/detour：完整 listener 与全部可达 outbound 必须在 custom merge 后仍保持绑定。该断言不得遮蔽主入口的上游规则。
- `auto_detect_interface` 仅是底层 OS 接口选择，不能改变入口到逻辑线路的绑定，也不能作为“Mixed 自动选线路”的验收依据。
- 本机 Clash TUN 存在时，本地远端成功只能证明请求完成，不能单独证明物理出站归属，也不能排除代理套代理/Fake-IP 影响。此时按 [Clash TUN 共存](../operations/CLASH_TUN_COEXISTENCE.md) 和 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md) 做对照。

L2 历史诊断细节见[已验证基线](OPENWRT_REMOTE_LAB.md#已验证基线)。2026-07-20 的旧探针对每个临时变体强制 `auto_detect_interface=true`；它只能在这个共同条件下把故障收敛到 AnyTLS mihomo client 与 `g-2` detour 的组合，不能证明产品导出接口策略。必须按当前默认 preserve 重跑后才能成为现行证据，更不证明 Windows Wintun、WFP、系统代理、GUI 或线路重启已通过。

## 发布前最小矩阵

1. 在干净的 Windows 用户目录启动，不复用唯一真实配置，不停止或改写 Clash TUN。
2. 覆盖空配置、旧配置迁移、损坏配置和恢复流程。
3. 完成 Mixed 三种协议、认证正反例、端口占用、非法端口与主/辅端口映射。
4. 完成 Trojan、AnyTLS 及有/无 front proxy；L2 只能补充远端归因，最终仍需 L3 Windows 证据。
5. 验证只有用户手动操作才能启停系统代理/TUN；GUI 退出、重启和线路切换不改变它们。
6. 分别核对 TUN requested、worker-observed 和 Windows 实际状态；core 崩溃后当前只恢复空控制 core的行为必须被识别为未通过，而不是把 requested 误判为 TUN 已恢复。
7. 在保护持续生效时完成线路重启和故障注入，证明不会 direct 泄漏；提交后故障必须进入阻断，不得自动切回旧线路。只有用户明确选择、旧 generation 重新验证后才测试显式回滚。
8. 覆盖 worker 无 instance、worker kill、service/BFE restart，确认 TCP、UDP、HTTP helper 和物理 IPv4/IPv6/DNS 均无系统 fallback。
9. 覆盖空订阅、HTML 错页、超时、解析失败和正常更新；失败时旧组原样保留。
10. 覆盖更新失败、校验失败和回滚；修复更新器前不得进行真实更新测试。
11. 对 GUI→Client→core 注入 Exit ACK 丢失、finished 先到/后到、在途 handler 卡住、异常进程退出和重复关闭请求；仅 exact non-admission 可恢复，其余必须保持 continuation fence，且全过程核对 Windows 实际路由/DNS/TUN/WFP。

## 工具入口

- `tools/export_profile_core_config.ps1`：导出单个 profile 的实际 core 配置并可执行 `check`。
- `tools/verify_mixed_inbound.ps1`：只接受主 `mixed-in -> proxy` 连通性诊断；拒绝 TUN/系统 NTP 写入/endpoints、裁剪到 `proxy` 的精确 detour 闭包后启动临时配置，验证监听 PID 和三种代理请求；不是辅助映射 contract 或 Windows 集成验收器。
- `tools/verify_mixed_openwrt.py`：在固定 `192.168.1.7`、`127.0.0.1:52080` 和唯一临时目录中执行相同的严格收紧与 L2 探针；必须先 `--dry-run`，真实结果须满足远端基线未变和清理完成。
- `test/fixtures/mixed-direct-sanitization.json`：验证诊断脚本不会启动额外 LAN inbound/controller、修改系统代理或写配置指定日志。
- `test/test_mixed_probe.ps1`：用 loopback HTTP 204 origin 运行 direct、dummy auth 与安全拒绝 fixtures，并核对端口、系统代理、文件副作用和 origin 清理；不依赖公共站。2026-07-20 为 7/7。
- `test/test_verify_mixed_openwrt.py`：覆盖远端收紧器的接口字段、NTP/endpoints、精确 detour 闭包、敏感摘要与命令安全；当前 19/19。
- `test/test_runtime_connectivity.ps1`：在临时 package 和 loopback HTTP 204 origin 中验证运行快照脚本的 PID 归属、精确 HTTP 状态、错误期望拒绝及清理；2026-07-20 的 204 正例通过，错误期望 200 正确失败。
- `test/config_recovery_test.cpp`：覆盖单/多文件恢复基础设施、`VerifiedBefore`/`VerifiedAfter`/`Indeterminate`、退役、hidden/unexpected/exact-case/staging，以及 terminal startup/report 分层校验。路径用例 `routes_box/ROUTE~1` 只证明 `~` 被词法规则拒绝，不表示测试构造了真实 Windows 8.3 alias；选定配置根本身的 junction/别名仍需操作者确认。
- `test/runtime_transition_test.cpp`：覆盖 GUI process-local transition/queue/crash generation 竞态、daemon generation 与 UUID 同锁快照，以及 `{generation, UUID, PID}` finished tracker 的错误身份/PID拒绝、异常完成、重复 finished 和 finished-before/after-wait；它不创建 QProcess、GUI、core 进程，不执行真实 HTTP/2，也不覆盖 `TrafficData` 并发或 Windows TUN/WFP。
- `test/share_format_test.cpp`：只用假凭据覆盖原生链接 fragment 精确删除、字面 IPv4、端口、SOCKS5/HTTP、TLS、认证与分隔符正负例；不创建 MainWindow 或操作真实剪贴板。
- `test/core_exit_integration_test.cpp`：只由完整无 Skip package 授权运行；再次校验 fresh package core 的规范路径/SHA-256/非生产 identity，以 `NoProxy` raw Qt HTTP/2 client 和 test-owned QProcess 验证协议 v3 deadline/non-admission 对账 fence、迟到 Exit 拒绝和正常退出。配置无 listener、无 TUN；失败清理先尝试已鉴权 Stop/Exit，最后才可 terminate/kill 精确 test-owned PID。它不是 GUI→Client E2E，只比较 WinINet 的 `ProxyEnable`、`ProxyServer`、`AutoConfigURL`、`ProxyOverride`、`AutoDetect` 五键。
- `go/cmd/nekobox_core/core_lifecycle_test.go`：覆盖失败/取消 candidate、blocked Close、旧 reference、dial/stats/Stop 互斥、并发 Start、deadline 准入 fence、Exit STOPPED 前置与终态 `EXITING`；另覆盖 reconcile barrier 先挡迟到 Start/Exit、等待阻塞 Start/Stop、精确 active/failed-clean/blocked target、config hash 与 ordering watermark。`grpc_box_test.go` 覆盖 deadline/Exit/对账映射；`grpc_exit_integration_test.go` 通过真实 localhost gRPC 验证排队 Stop deadline 和 ACK 交付后 GracefulStop。`go/grpc_server/auth` 和 `grpc_identity_test.go` 覆盖 token + daemon UUID、协议 v3、one-shot shutdown controller、metadata 清除和握手回显。
- `go/cmd/nekobox_core/internal/boxapi/boxapi_test.go`：除无 instance fail-closed 外，覆盖 generation-bound HTTP transport 禁用 keep-alive，防止连接跨代复用。
- `test/test_config_preservation.ps1`：在隔离 appdata 中启动配置导出路径，验证已存在但损坏的主/路由配置、重复/非字符串/错误 JSON 类型的辅助映射、非法活动路由路径、未知 profile 类型与悬空 group 引用保持 SHA-256 不变，并验证 snapshot/metadata；同时验证显式事务报告与 before 回滚，以及 pending 事务在加载前阻断且主配置不变，当前为 10/10。
- `test/test_final_config_guards.ps1`：在隔离临时 appdata 中验证安全 `internal-full` 文件导出、部分 TUN/系统代理/system endpoint/NTP 副作用拒绝，以及标准 SOCKS profile 的测试态 custom、Managed Mixed listener、detour 禁改和 custom route 空字段回归；2026-07-20 为 10/10。不启动线路，也不覆盖 live/test 的 TUN on/off 四象限、export 删除字段或所有 NTP/direct-action dialer，不能代表完整 C++ golden。
- `tools/verify_runtime_connectivity.ps1`：采集整套运行时快照；不是自动验收器。
- `tools/verify_fail_closed_restart.ps1`：采集 fail-closed 相关状态；使用限制见 [fail-closed 验证](FAIL_CLOSED.md)。

这些工具调用的 `nekobox_core check/run` 是[显式高级 CLI](../reference/CLI.md#core-高级-cli)，会直接读取经过导出或收紧的临时配置；它们不走普通 GUI，也不能用于证明 Go 层已经独立实施产品策略。
