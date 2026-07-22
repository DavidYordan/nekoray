# 接管状态

状态：Alpha / 不可发布
基线：NekoRay 4.0.1 `adef6cd` → `96f1166`，现行接管分支 `agent/takeover-remediation`
最后更新：2026-07-22

## 结论先行

上一阶段已经明显偏离需求：它不仅增加 AnyTLS、并发线路和 Clash server-domain DoH，还删除了多项 NekoRay 能力、加入 MultiMapper 与复杂 resolver 工具，并用“禁止 TUN 下重载”代替无泄漏切换。因此当前分支不能按既有方向继续堆功能，必须先恢复最小分支边界。

“Mixed 无法连接”包含两个不同问题，不能再混为一谈。隔离 core 测试证明 Mixed listener 本身能接收 HTTP、CONNECT、SOCKS5h 并进入指定 outbound；但旧 GUI 只在 stdout 查找 ready 文本，而 Go `log.Printf` 实际写 stderr，因此控制面确实可能永远不发送 Start、`12080` 根本不监听。本批已改成 stdout/stderr 仅触发 UUID/协议握手，精确握手后才 ready；尚缺真实 GUI/QProcess 集成测试。profile 真正启动后的既有主配置故障则仍集中在 **AnyTLS(Mihomo client) 经 Trojan detour** 的组合链。

## 审计结果

`adef6cd..96f1166` 有 36 个提交、约 117 个文件变化、约 +8,497/-3,236 行。主要偏离为：

| 等级 | 发现 | 处理方向 |
|---|---|---|
| P0 | external-core、Naive、custom external、TUIC/Hysteria2 外核被整体删除 | 选择性恢复；仅 Xray 保持删除 |
| P0 | 未知/现已不识别 profile 曾被 loader 删除 | 已停止删除与 ID 复用，并生成可验证 quarantine 证据；事务 CLI 只处理结构化事务，不会自动恢复 unknown/quarantine，仍缺图形恢复与未知模型修复 |
| P0 | 订阅刷新曾在验证前改 order/删 profile | 已改为 parse/stage/validate 后提交；继续补多文件事务与故障注入 |
| P0 | TUN 下重载被 UI 阻止；整核 Stop/Start 无独立 kill-switch | 进程内 lifecycle/generation 止损已落地；仍需持久 runtime + WFP + OS 级 generation 事务 |
| P0 | final custom 曾可覆盖 Mixed 端口绑定；空辅助 chain 曾可 fail-open | 最终 validator 已落地；补完整负向自动回归 |
| P0 | DoH 扩展曾含普通 nameserver 猜测、本机 fallback、无 DoH也套自定义 group | 已收敛为精确 proxy-server-nameserver + strict；bootstrap 仍待实现 |
| P1 | VMess/v2rayN、SOCKS userinfo、SS v2ray-plugin 等格式兼容误删 | 恢复，不等同于恢复 Xray core |
| P1 | GeoSite 自动完成变成空 UI；测试路径曾出现直连/无界风险 | URL Test 已恢复为有界配置；TCP Ping 已禁用；GeoSite 仍待恢复 |
| 越界 | MultiMapper、复杂批量解析/改 IP、通用健康探测平台 | 退出产品主线，材料归档 |

完整逐提交证据见 [范围偏离审计](archive/audits/2026-07-20-scope-deviation-audit.md)。

## Mixed 与 AnyTLS 的真实证据

OpenWrt `192.168.1.7` 使用同版本 `sing-box 1.13.12-routefluent-anytls-client.7` 的临时 loopback 探针，未修改现有 RouteFluent runtime：

| 配置 | HTTP | CONNECT | SOCKS5h | 结论 |
|---|---:|---:|---:|---|
| AnyTLS `mihomo/1.19.28` + `detour=g-2` | 失败 | 失败 | 失败 | AnyTLS create-session EOF |
| 同一 AnyTLS，仅移除 detour | 204 | 204 | 204 | AnyTLS/Mihomo 单跳可用 |
| 移除 detour并改 native | 失败 | 失败 | 失败 | 服务端 internal error |
| 独立 Trojan profile 2（与 `g-2` 对象相同） | 204 | 204 | 204 | Trojan 单跳可用 |

因此临时 loopback Mixed、目标 outbound 的 Mihomo AnyTLS 单跳和 Trojan 单跳分别可达；组合 detour 尚未闭环。探针固定改写为临时 `52080` 并显式指向目标 outbound，不能证明产品 `12080` 主端口映射。旧探针还会在临时 OpenWrt 副本强制 `auto_detect_interface=true`，所以这些结果只能用于协议组合归因，不能证明导出配置的接口策略；工具现已改为默认 preserve，后续需按新默认重跑。

## 已完成的接管止损

- 明确产品边界：保留 NekoRay，只有 Xray 明确删除；新增仅三项。
- 产品生成器已精确恢复上游 `auto_detect_interface=dataStore->spmode_vpn`：live 与 test 都只随产品 TUN 意图变化，文件 export 随后删除该字段；没有生产 NekoRay、物理网卡或本机双 TUN 特例。三份 loopback 诊断 fixture 也已移除无必要的 `true`。尚缺 live/test 的 TUN on/off 四象限与 export 删除边界的 C++ golden，不能把源码审计写成自动验证完成。
- OpenWrt 探针默认保留该字段，仅显式诊断参数可强制；收紧器会拒绝系统 NTP 写入/非空 endpoint，并把 outbound 缩到目标线路的精确 detour 闭包；当前 Python 工具单测 19/19 通过。
- 主 Mixed 默认 `12080`；Clash API 默认保持关闭（配置值 `-9090`，启用时端口 `9090`）；`2080` 与生产安装完全隔离。
- 主/辅助 Mixed 标准路径显式绑定各自 chain；辅助 chain 失败、profile失效、端口重复现在整体构建失败，不再留下孤儿入口。
- 主/辅助 Mixed 都不再在精确线路绑定前借全局/默认 DNS 执行 `resolve`；使用 provider server-domain DoH 的 outbound 会绑定精确 strict resolver。无 provider DoH 的普通节点仍走 NekoRay/sing-box 原有解析路径。
- 没有 provider DoH 的普通节点不再强制使用自定义 `local_only` resolver group。
- Clash 导入只读取 `proxy-server-nameserver`/`proxy_server_nameserver`；字段存在但没有有效 HTTPS DoH 时整次导入失败且旧数据不变。
- 生成配置已移除 local fallback/probe；DoH endpoint 仍是域名且没有可审计 bootstrap 时明确构建失败，不再偷用 `local-system`。
- 顶层 custom 合并前会捕获每个受管 Mixed 的完整生成 listener 和沿 detour 可达的全部 outbound 对象；合并后要求这些对象逐项相同、各 tag/port 唯一且精确无条件 route 绑定仍在所有可能改投/提前 resolve 规则之前。profile 级 `custom_outbound` 可在快照前修改普通字段，但不得新增或改变 detour。provider resolver 的 outbound → strict group → 精确 DoH server 也按生成对象锁定，并拒绝 RouteFluent fallback/local-only 字段。
- 辅助端口 map 不再因 stop/restart/crash/exit 或 UI 刷新被清空；字段类型错误、非字符串、损坏或重复项会使既有主配置原件保持不变并中止启动。显式启停/删除映射只有在原子保存成功后才继续 reload，失败会回滚内存。
- 普通单文件保存使用禁止 direct-write fallback 的 `QSaveFile`，与多文件事务共享提交串行化 mutex 和跨进程磁盘锁。每次实际内容变化在 commit 前发布短生命周期 durable before/after intent；精确验证 before/after 后写成 `aborted`/`committed`、移到 `recovery/retired-single-file-transactions/` 并尽力删除，无法判定则保留 `prepared` 并阻断。加载时存在性也进入快照，旧进程不能把被外部删除的文件当作新文件重建。覆盖前建立可验证备份，损坏/未知 profile 原件保留并生成 quarantine，且 ID 不复用，危险 reorder 已禁止。group 创建会同时提交 group 文件与 `pm.json`；单 profile/空 group 删除和 profile 跨组移动已接入持久 before/after manifest 与失败逆序回滚。启动扫描到完整配置加载由同一可重入磁盘锁覆盖；锁不会仅因运行超过 30 秒被抢占。扫描会枚举隐藏/系统项，拒绝 active lock、意外条目、manifest/身份/header 问题和非终态状态；终态只校验 JSON 与 schema/id/state header，合法终态即使 entries 损坏也不阻断启动，但 report 会深解析并标为 `valid=false`，非法终态 schema 仍阻断。命令行可先生成报告，再由用户明确选择结构化事务的完整 before/after，恢复中途不允许改方向；它不自动恢复 unknown/quarantine。配置根以下会拒绝危险 Windows 名、大小写重复和 reparse/junction，选定根本身是必须由操作者确认非 junction/别名的信任锚。删除对象会 tombstone，测速线程迟到保存不能复活文件；运行中的 auxiliary 也拒绝删除。非空 group 的旧半删除路径仍整体禁用。
- route 名现限制为安全 Windows 单文件名，非法 `active_routing` 保留主配置原件并中止加载；非活动 route 使用事务删除，活动 route 必须先显式切换，不再直接删除后尝试补救。
- 订阅刷新恢复为 parse/stage-before-mutate；解析失败、空响应、坏结构与零支持节点不写入。联网前会按值快照不可变 HTTP 选项，并记录目标 group 与全部成员的身份、顺序、tombstone 和序列化状态；提交回 UI 线程并取得提交串行化 mutex 后逐项重验，完成回调也回到 UI 线程。清理/回滚删除失败会保留对象并明确记录，但成功候选仍由多个独立 profile/group 提交组成，尚未形成跨文件崩溃一致事务。
- ConfigBuilder 不再为统计回写 live `TrafficData.id/tag`，VLESS core 对象生成也不再为规范化 `-udp443`/`none` 改写 bean `flow`。group speedtest 现由 UI 线程生成 immutable job，按值冻结 profile/bean/最终 test config fingerprint 与请求；worker 只执行 RPC，结果回 UI 后以对象身份和 fingerprint 重验再保存，迟到结果不能覆盖已编辑、替换或有效配置已变化的 profile。`TrafficData::last_update` 已初始化为 `0`，消除了首次速率计算的未初始化读取；counter/rate 的 worker 写入与 UI/JsonStore 无锁读取仍是 P2，并未被 `TrafficBinding` 解决。该项只是完整 `BuildModelSnapshot` 前的局部止损。
- Go helper 在无有效 sing-box instance 时不再回落系统 TCP/UDP/HTTP 网络。core candidate 现在只在启动成功后按 generation 发布；Start/Stop/dial/stats/Exit 由 lifecycle mutex 串行，旧 generation reference/HTTP client 与 Close 不确定形成的 blocked generation 均 fail closed，generation-bound HTTP client 禁止连接跨代复用。
- Windows legacy 外置 TUN 因无精确 PID/句柄且曾按映像名批量 `taskkill`，现已安全禁用；配置数据保留，默认内部 TUN不受此项删除。
- Windows 内部 TUN 生成配置现强制 `strict_route=true` 并同时覆盖 IPv4/IPv6；这只收紧活动期，仍不能覆盖 worker/GUI 消失窗口。
- 最终 OS-mode validator 会拒绝未由显式产品 TUN 开关生成的 TUN，并要求受管 TUN 完整对象与生成值精确一致；在受管 TUN 活动配置中还拒绝 `route.default_interface`，以及 outbound/endpoint/DNS/NTP 和嵌套 route `action=direct` dialer 的 bind-address/interface 覆盖。任意模式下的 inbound `set_system_proxy=true`、系统 WireGuard/Tailscale endpoint 与 NTP 写系统时钟均拒绝。它是已知 OS 副作用与产品 TUN 对象的窄 guard，不代表任意 `route.rules` 都已形式化证明安全。
- 无令牌的 `-flag_restart_tun_on` 自动提权连续流程已删除；非管理员用户须自行以管理员身份启动后再次手动启用 TUN。
- legacy WinINet 系统代理接口不能证明写入成功、所有权或完整恢复原 PAC/proxy/bypass，产品内 Windows 系统代理切换现已临时禁用，等待按 SID 的 compare-and-restore broker。
- MultiMapper 专用导出和复杂批量 resolver/change-IP 平台已从产品 UI/构建/代码移出，历史材料保留在 archive。上游简单 **Resolve domain** 因直接走 Windows 系统 resolver 并永久覆盖节点域名，现也改为无副作用禁用说明；未来只能经对应 provider resolver 且保留原域名。
- 打包脚本不再关闭/强杀运行实例；发现运行即失败。生产安装不再是默认构建参考目录。构建、GUI/core 夹具、配置导出和运行审计的可写/可执行/临时路径共用保守的生产路径护栏：规范化后只允许本地固定磁盘的盘符路径，拒绝 UNC/设备路径、ADS/额外冒号命名空间、网络映射/可移动盘、SUBST/DOS 设备重定向、8.3 短路径和 ReparsePoint/junction，并识别指向与 `D:` 相同物理卷的盘符别名。它仍没有通过最终句柄核验 final-file identity，不能宣称解决所有已存在 hardlink 等同文件别名；单文件导出/报告工具对已存在目标采取拒绝覆盖策略，但这只是附加止损。
- 启动/普通重启不再根据 remember 状态或 CLI 连续参数自动启用系统代理/TUN；core 崩溃时只重启空控制 core，不自动恢复 profile/TUN。
- GUI Start/Stop/CrashCleanup 现由单一 process-local transition ticket 串行；coordinator 同一 mutex 内同步 participating-mutation depth gate，pending crash cleanup 直接 handoff。每次 `CoreProcess::Start` 生成新 UUID 并传给 core；所有 RPC 在 handler 前同时验证 session token 与精确 UUID。`grpc server listening` 只触发 `GetDaemonInfo` 身份/协议握手，成功后才把同一 `{QProcess generation, UUID}` 标为 ready；旧进程 500 ms 内未确认退出时拒绝发布 replacement identity。Start/Stop/Exit 使用 GUI session 单调 command sequence，Go lifecycle 在同一 mutex 内排序；Start/Stop 还记录服务端 config SHA、target outcome 与 active Start sequence。响应不确定时，GUI 用更高 sequence 的 `ReconcileLifecycle` barrier：目标先执行则等待终态，barrier 先执行则迟到目标变 stale。只有精确 active/stopped 结论才推进 UI，其余继续 indeterminate。TrafficLooper binding 仍只在成功 commit 后发布；profile/TUN 不自动恢复。
- `DataStore::core_running`/`prepare_exit` 已改为 atomic bool，CoreProcess 不再跨线程读取 `spmode_vpn`，异常退出统一只重启空控制 core。退出/重启会等待 Stop completion；失败或对账仍不确定时不调用 core Exit/GUI quit，而是撤销退出控制并保留 observed runtime。Start/Stop HTTP/2 client 的 30 秒 abort 与随后的 5 秒对账等待都只界定 GUI 等待，Go handler/屏障不会据此被取消；再次超时仍不是端到端 deadline。Exit 尚未 ACK 并等待精确 UUID 的 QProcess finished。
- Windows GUI 已忽略 CRT `SIGTERM`/`SIGINT`，避免控制台信号直接绕过 UI 退出 guard；这不处理 `TerminateProcess`/任务管理器强制结束、崩溃、关机或 worker 自退，持久 Runtime/WFP 仍是 P0。
- URL/Full Test 只接受精确有界的临时生成配置，`internal-full` 与顶层 `custom_config` 变更会被测试路径拒绝；产品 TUN requested/worker active 任一成立时拒绝新测试，测试运行中也拒绝启 TUN。Full Test 中原先调用系统 DNS 的“入口 IP”查询已禁用，并补上空配置/非法 URL 拒绝、父 RPC context 取消、超时 goroutine 防阻塞和 64 KiB 响应上限；TCP Ping 在 GUI 与 core 两层都明确禁用，因为它使用系统直连 socket。
- 默认文件导出和 `for_share` 别名都使用无产品 TUN/辅助运行态的审计导出；`for_test` 使用独立有界测试配置。所有模式都执行已知 OS 副作用校验，但导出文件仍含凭据且不能视为可任意启动的沙箱。
- 普通 GUI 通过 localhost、每个 GUI session 随机令牌的 gRPC 控制 core；token 在同一会话内沿用，每次 daemon 启动另有不可变 UUID fence，旧协议组合 fail closed。UUID 不是配置授权、持久 generation 或 OS owner。独立 `nekobox_core run/check` 是构建与隔离测试显式使用的高级入口，可直接读取 sing-box 配置；Go 层尚未重复 C++ 的产品策略校验，属于需要补齐的纵深防御边界，而不是普通 GUI 可随意绕过 guard 的证据。
- 本轮已用当前源码完成一次不带 Skip 参数的本地完整打包；`build-package-windows64/`、`deployment/windows64/` 与 zip 中的 GUI/core 来自同一轮构建。deployment/zip 仍是忽略的本地验收产物，不可交付。

## 2026-07-22 无侵入回归快照

- 当前源码的 Windows 全量 C++/package 本地重编译成功。Go core 已通过普通测试、`go test -count=20 ./...`、`go vet ./...` 与 `go test -race ./...`；`grpc_server` 已通过普通测试、vet 与 race。它们证明本轮源码和进程内并发断言，不是父进程/Windows TUN/WFP 生命周期验收。
- 本轮本地审计二进制快照：`nekobox.exe` SHA-256 `973671A7C3EB6882350945A35A2DF38ACCB700A950A7B36A4BE7FB79010E51EF`；`nekobox_core.exe` SHA-256 `1556618A46FAD2CDF88281DFB6194F202417EA00FBFE25DE44C8EB5AAE8C2BF4`；zip SHA-256 `D8410F7E7930D4DC204B68D2E913A192C5777E68EC83894FA5B048B25C11605C`。当前版本完整 Windows 打包脚本已无 Skip 参数实跑成功，`deployment/windows64/` 与 zip 已在本地刷新；212 个 package 配置文件恢复完成，两个 preserve 目录均已清除。deployment/zip 仍是忽略的本地验收产物，不是 release manifest，也不改变 Alpha/不可发布判断。
- `test_final_config_guards.ps1` 10/10，`test_config_preservation.ps1` 10/10，OpenWrt helper Python 单测 19/19。
- 仓库卫生会解析所有已跟踪 PowerShell，并自测生产路径 exact/subtree/short-name/UNC/device/ADS/SUBST/reparse 等拒绝分支；本轮另以生产 GUI/core 路径参数验证两个测试入口均在启动前失败。这些用例不覆盖 hardlink/final-file identity，不得将其解读为完整的物理文件别名验收。
- 本地 Mixed fixture 7/7；额外 listener、系统代理、禁用日志和 loopback origin 清理均保持预期。
- runtime connectivity 的 expected 204 场景通过，HTTP 与 SOCKS5h 均为 204；expected 200 场景按预期报告 2 项 mismatch 并返回失败。系统代理、fixture 端口和 origin 清理均通过。
- 本轮已在项目 MinGW `bin` 已加入 `PATH` 的环境中执行 `ctest --test-dir build-package-windows64 --output-on-failure`，2/2 通过。`config_recovery` 覆盖事务基础提交、模拟回滚、锁、pending 阻断、before/after 恢复、方向锁、单文件 `VerifiedBefore`/`VerifiedAfter`/`Indeterminate` intent、退役目录、隐藏/意外条目、精确大小写身份和协议 staging，并锁定 terminal startup/report 分层边界；其中 `routes_box/ROUTE~1` 只是 `~` 的词法拒绝用例，不代表真实 8.3 alias。`runtime_transition` 覆盖 process-local transition fencing、crash-cleanup handoff、daemon/profile-request generation、queue-or-ready 顺序与真并发恰好一次等纯状态行为；它不启动 QProcess/GUI/core、真实 HTTP/2 超时或 Windows TUN/WFP。两者都不覆盖订阅/非空 group、完整 ConfigBuilder snapshot/golden，也不改变 Alpha/不可发布判断。

## 仍然阻断发布

1. external-core/Naive 与误删格式兼容尚未恢复；未知数据已保留并生成 quarantine，结构完整的事务已有显式命令恢复，但图形恢复、未知模型修复仍缺，非空 group 删除因此暂时拒绝。
2. 单文件原子保存、group 创建与部分删除/移动事务已落地，订阅也已先 stage、在 UI 线程串行提交并报告删除失败；订阅成功候选仍未接入单一事务，也缺真实进程终止/磁盘故障和图形恢复验收。
3. 最终 Mixed、strict resolver 与 TUN/系统代理副作用不变量校验已落地，并新增 `test/test_final_config_guards.ps1` 覆盖部分导出拒绝分支；完整 C++ 配置生成 golden/负向回归仍未完成，不能只凭脚本存在判定通过。
4. AnyTLS + Trojan detour 仍失败。
5. 当前 Go wrapper 没有原地热重载；内部 TUN 与单个 Box 同生共死。进程内 lifecycle/generation 已封住直接 Start/Stop/旧引用竞态，但 Windows Runtime Service、stable Mixed/TUN anchor 和 persistent WFP kill-switch 尚不存在；GUI/父进程死亡仍会带走当前数据面。
6. Windows 下主+多辅助真实不同出口、TUN 切线、worker/GUI 崩溃窗口与 IPv4/IPv6/DNS 防泄漏尚未验收；当前 guard 仍违反“开 TUN 时可切线”。
7. 精准系统代理 broker 尚未实现，产品内切换暂禁；旧 WinINet helper 不得重新接回 UI。
8. Go core 仍缺产品策略的第二层校验。process-local lifecycle mutex/generation、每 daemon UUID、全 RPC identity validation、ready handshake、command sequence 与 indeterminate 对账 barrier 已完成；它们可封住 reused port/token 和响应丢失的已知竞态。但 RPC 仍无服务端可取消的端到端 deadline、持久 RuntimeStateMachine 或 Windows OS 事实源；对账自身超时仍为 unknown，Exit 也未等待精确进程退出。token/UUID 只保护调用者与实例身份，不解决配置授权或 OS 状态所有权。已有纯状态/race 测试仍需补 QProcess/GUI crash→commit/退出、真实 HTTP/2 超时和 Windows TUN/WFP 集成。
9. 当前提交串行化 mutex 不是完整模型读写锁；ConfigBuilder 两类明确 live-model 写入和 group speedtest worker 读写已止损，但完整 `BuildModelSnapshot`、订阅与其它跨线程读取仍未完成。final Start gate 只复核 recovery、ticket 和已捕获 daemon readiness，不重建 candidate 或比较完整 model revision。route/settings/hotkey 等调用也尚未统一处理保存失败与内存回滚，均属于 Alpha/P0 数据一致性债务。
10. Windows-only CI 已建立并通过，本机完整无 Skip 打包也已刷新忽略的 deployment/zip；但干净 Qt/MinGW/C++ 依赖工具链、真实交付二进制 manifest 和 Windows 集成验收尚未完成。libneko 已锁定为仓内子模块。

另有一项不冒充当前 P0 发布主阻断的 P2 债务：`TrafficData::last_update` 的未初始化读取已修复，但 counter/rate 仍由 worker 写、UI/JsonStore 无统一同步读取；需后续采用不可变遥测快照或明确锁/原子设计。

本分支不得部署到 `D:\Program Files\nekoray`。后续顺序见 [推进路线](ROADMAP.md)。
