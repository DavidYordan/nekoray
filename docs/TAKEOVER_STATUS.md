# 接管状态

状态：Alpha / 不可发布
基线：NekoRay 4.0.1 `adef6cd` → `96f1166`，现行接管分支 `agent/takeover-remediation`
最后更新：2026-07-22

## 结论先行

上一阶段已经明显偏离需求：它不仅增加 AnyTLS、并发线路和 Clash server-domain DoH，还删除了多项 NekoRay 能力、加入 MultiMapper 与复杂 resolver 工具，并用“禁止 TUN 下重载”代替无泄漏切换。因此当前分支不能按既有方向继续堆功能，必须先恢复最小分支边界。

“Mixed 无法连接”包含两个不同问题，不能再混为一谈。隔离 core 测试证明 Mixed listener 本身能接收 HTTP、CONNECT、SOCKS5h 并进入指定 outbound；但旧 GUI 只在 stdout 查找 ready 文本，而 Go `log.Printf` 实际写 stderr，因此控制面确实可能永远不发送 Start、`12080` 根本不监听。本批已改成 stdout/stderr 仅触发 UUID/协议 v3 握手，精确握手后才 ready；完整 package 的 raw QProcess/Qt HTTP/2 gate验证 core 协议，但不调用 GUI Client/ready 状态机，所以仍缺真实 GUI→Client 握手测试。profile 真正启动后的既有主配置故障则仍集中在 **AnyTLS(Mihomo client) 经 Trojan detour** 的组合链。

## 审计结果

`adef6cd..96f1166` 有 36 个提交、约 117 个文件变化、约 +8,497/-3,236 行。主要偏离为：

| 等级 | 发现 | 处理方向 |
|---|---|---|
| P0 | external-core、Naive、custom external、TUIC/Hysteria2 外核被整体删除 | 选择性恢复；仅 Xray 保持删除 |
| P0 | 未知/现已不识别 profile 曾被 loader 删除 | 已停止删除与 ID 复用，并生成可验证 quarantine 证据；事务 CLI 只处理结构化事务，不会自动恢复 unknown/quarantine，仍缺图形恢复与未知模型修复 |
| P0 | 订阅刷新曾在验证前改 order/删 profile | 已改为 parse/stage/validate 后提交；继续补多文件事务与故障注入 |
| P0 | TUN 下重载被 UI 阻止；整核 Stop/Start 无独立 kill-switch | 进程内 lifecycle/generation 止损已落地；仍需持久 runtime + WFP + OS 级 generation 事务 |
| P0 | final custom 曾可覆盖 Mixed 端口绑定；空辅助 chain 曾可 fail-open | 最终 validator 已落地；补完整负向自动回归 |
| P0 | DoH 扩展曾含无条件 nameserver 猜测、本机 fallback、无 DoH也套自定义 group，随后又误拒域名 DoH | 已按字段存在性三态修正：WD 原生、NEX 自带 DoH；strict provider resolver + 原生 endpoint bootstrap |
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
- Clash 导入按字段存在性选择唯一 resolver 来源：专用 `proxy-server-nameserver` 显式存在时权威、不借普通 nameserver；专用字段 absent 时才提取 `dns.nameserver` 的 HTTPS DoH。所选来源的非法 HTTPS 项使刷新失败且旧数据不变，非 HTTPS 项只计数；来源和策略版本随 group 保存。
- 生成配置已移除 provider local fallback/probe。域名 DoH endpoint 使用 NekoRay 原生 `dns-local` bootstrap，保留 TLS SNI、不强制地址族；它只建立 DoH 传输，不会在 provider DoH 失败后解析线路 server。无 provider DoH 的节点走原生解析。
- 顶层 custom 合并前会捕获每个受管 Mixed 的完整生成 listener 和沿 detour 可达的全部 outbound 对象；合并后要求这些对象逐项相同、各 tag/port 唯一且精确无条件 route 绑定仍在所有可能改投/提前 resolve 规则之前。profile 级 `custom_outbound` 可在快照前修改普通字段，但不得新增或改变 detour。provider resolver 的 outbound → strict group → 精确 DoH server → 原生 bootstrap 也按生成对象锁定，并拒绝 RouteFluent fallback/local-only 字段。旧策略留下的非空订阅 DoH 在成功刷新前拒绝构建。
- 辅助端口 map 不再因 stop/restart/crash/exit 或 UI 刷新被清空；字段类型错误、非字符串、损坏或重复项会使既有主配置原件保持不变并中止启动。显式启停/删除映射只有在原子保存成功后才继续 reload，失败会回滚内存。
- 普通单文件保存使用禁止 direct-write fallback 的 `QSaveFile`，与多文件事务共享提交串行化 mutex 和跨进程磁盘锁。每次实际内容变化在 commit 前发布短生命周期 durable before/after intent；精确验证 before/after 后写成 `aborted`/`committed`、移到 `recovery/retired-single-file-transactions/` 并尽力删除，无法判定则保留 `prepared` 并阻断。加载时存在性也进入快照，旧进程不能把被外部删除的文件当作新文件重建。覆盖前建立可验证备份，损坏/未知 profile 原件保留并生成 quarantine，且 ID 不复用，危险 reorder 已禁止。group 创建会同时提交 group 文件与 `pm.json`；单 profile/空 group 删除和 profile 跨组移动已接入持久 before/after manifest 与失败逆序回滚。启动扫描到完整配置加载由同一可重入磁盘锁覆盖；锁不会仅因运行超过 30 秒被抢占。扫描会枚举隐藏/系统项，拒绝 active lock、意外条目、manifest/身份/header 问题和非终态状态；终态只校验 JSON 与 schema/id/state header，合法终态即使 entries 损坏也不阻断启动，但 report 会深解析并标为 `valid=false`，非法终态 schema 仍阻断。命令行可先生成报告，再由用户明确选择结构化事务的完整 before/after，恢复中途不允许改方向；它不自动恢复 unknown/quarantine。配置根以下会拒绝危险 Windows 名、大小写重复和 reparse/junction，选定根本身是必须由操作者确认非 junction/别名的信任锚。删除对象会 tombstone，测速线程迟到保存不能复活文件；运行中的 auxiliary 也拒绝删除。非空 group 的旧半删除路径仍整体禁用。
- route 名现限制为安全 Windows 单文件名，非法 `active_routing` 保留主配置原件并中止加载；非活动 route 使用事务删除，活动 route 必须先显式切换，不再直接删除后尝试补救。
- 订阅刷新恢复为 parse/stage-before-mutate；解析失败、空响应、坏结构与零支持节点不写入。联网前会按值快照不可变 HTTP 选项，并记录目标 group 与全部成员的身份、顺序、tombstone 和序列化状态；提交回 UI 线程并取得提交串行化 mutex 后逐项重验，完成回调也回到 UI 线程。清理/回滚删除失败会保留对象并明确记录，但成功候选仍由多个独立 profile/group 提交组成，尚未形成跨文件崩溃一致事务。
- ConfigBuilder 不再为统计回写 live `TrafficData.id/tag`，VLESS core 对象生成也不再为规范化 `-udp443`/`none` 改写 bean `flow`。group speedtest 现由 UI 线程生成 immutable job，按值冻结 profile/bean/最终 test config fingerprint 与请求；worker 只执行 RPC，结果回 UI 后以对象身份和 fingerprint 重验再保存，迟到结果不能覆盖已编辑、替换或有效配置已变化的 profile。`TrafficData::last_update` 已初始化为 `0`，消除了首次速率计算的未初始化读取；counter/rate 的 worker 写入与 UI/JsonStore 无锁读取仍是 P2，并未被 `TrafficBinding` 解决。该项只是完整 `BuildModelSnapshot` 前的局部止损。
- Go helper 在无有效 sing-box instance 时不再回落系统 TCP/UDP/HTTP 网络。core candidate 现在只在启动成功后按 generation 发布；Start/Stop/dial/stats/Exit 由 context-aware single-owner executor 串行，等待准入的命令可按 deadline 取消，Start 的取消/发布原子仲裁。旧 generation reference/HTTP client 与 Close 不确定形成的 blocked generation 均 fail closed，generation-bound HTTP client 禁止连接跨代复用。协议 v3 Exit 只从精确 `STOPPED` 原子进入 `EXITING`，返回结构化 ACK 后由 one-shot `GracefulStop` 结束 server；active/blocked 不会隐式 Stop。
- Windows legacy 外置 TUN 因无精确 PID/句柄且曾按映像名批量 `taskkill`，现已安全禁用；配置数据保留，默认内部 TUN不受此项删除。
- Windows 内部 TUN 生成配置现强制 `strict_route=true` 并同时覆盖 IPv4/IPv6；这只收紧活动期，仍不能覆盖 worker/GUI 消失窗口。
- 最终 OS-mode validator 会拒绝未由显式产品 TUN 开关生成的 TUN，并要求受管 TUN 完整对象与生成值精确一致；在受管 TUN 活动配置中还拒绝 `route.default_interface`，以及 outbound/endpoint/DNS/NTP 和嵌套 route `action=direct` dialer 的 bind-address/interface 覆盖。任意模式下的 inbound `set_system_proxy=true`、系统 WireGuard/Tailscale endpoint 与 NTP 写系统时钟均拒绝。它是已知 OS 副作用与产品 TUN 对象的窄 guard，不代表任意 `route.rules` 都已形式化证明安全。
- 无令牌的 `-flag_restart_tun_on` 自动提权连续流程已删除；非管理员用户须自行以管理员身份启动后再次手动启用 TUN。
- legacy WinINet 系统代理接口不能证明写入成功、所有权或完整恢复原 PAC/proxy/bypass，产品内 Windows 系统代理切换现已临时禁用，等待按 SID 的 compare-and-restore broker。
- MultiMapper 专用导出和复杂批量 resolver/change-IP 平台已从产品 UI/构建/代码移出，历史材料保留在 archive。上游简单 **Resolve domain** 因直接走 Windows 系统 resolver 并永久覆盖节点域名，现也改为无副作用禁用说明；未来只能经对应 provider resolver 且保留原域名。
- 打包脚本不再关闭/强杀运行实例；发现运行即失败。生产安装不再是默认构建参考目录。构建、GUI/core 夹具、配置导出和运行审计的可写/可执行/临时路径共用保守的生产路径护栏：规范化后只允许本地固定磁盘的盘符路径，拒绝 UNC/设备路径、ADS/额外冒号命名空间、网络映射/可移动盘、SUBST/DOS 设备重定向、8.3 短路径和 ReparsePoint/junction，并识别指向与 `D:` 相同物理卷的盘符别名。它仍没有通过最终句柄核验 final-file identity，不能宣称解决所有已存在 hardlink 等同文件别名；单文件导出/报告工具对已存在目标采取拒绝覆盖策略，但这只是附加止损。
- 启动/普通重启不再根据 remember 状态或 CLI 连续参数自动启用系统代理/TUN；core 崩溃时只重启空控制 core，不自动恢复 profile/TUN。
- GUI Start/Stop/CrashCleanup 现由单一 process-local transition ticket 串行；coordinator 同一 mutex 内同步 participating-mutation depth gate，pending crash cleanup 直接 handoff。每次 `CoreProcess::Start` 生成新 UUID 并传给 core；所有 RPC 在 handler 前同时验证 session token 与精确 UUID。`grpc server listening` 只触发 `GetDaemonInfo` 身份/协议 v3 握手，成功后才把同一 `{QProcess generation, UUID}` 标为 ready；旧进程未由精确 `QProcess::finished` 确认退出时拒绝 kill/replacement。Start/Stop/Exit 使用 GUI session 单调 command sequence，Go lifecycle 在同一 executor 内排序；Start/Stop 还记录服务端 config SHA、target outcome 与 active Start sequence。响应不确定时，GUI 用更高 sequence 的 `ReconcileLifecycle` barrier：目标先执行则等待终态，barrier 先执行则迟到目标变 stale。只有精确 active/stopped 结论才推进 UI，其余继续 indeterminate。TrafficLooper binding 仍只在成功 commit 后发布；profile/TUN 不自动恢复。
- `DataStore::core_running`/`prepare_exit` 已改为 atomic bool，CoreProcess 不再跨线程读取 `spmode_vpn`，异常退出统一只重启空控制 core。退出/重启先等待 Stop completion；失败或对账仍不确定时不发送 core Exit/GUI quit，而是撤销退出控制并保留 observed runtime。Stop 已确定后，GUI 冻结 `{QProcess generation, UUID, PID}`，要求精确 `EXITING` ACK，并持续等待同一进程 `NormalExit/0`；tracker 支持 finished-before/after-wait，等待期间不 kill/replacement，重复退出请求不能绕过 continuation fence。若 ACK 不可用，只有更高序号对账精确证明 `STOPPED/FENCED_NOT_ADMITTED` 才恢复控制，否则保持 fence 并继续等同一 PID finished。协议 v3 的标准 `grpc-timeout` 让服务端 deadline 早于 GUI abort；未准入命令可取消，Start candidate 在发布前可取消清理。已准入 Stop/Close、再次超时的对账和持续 finished wait 仍可能不确定。
- Windows GUI 已忽略 CRT `SIGTERM`/`SIGINT`，避免控制台信号直接绕过 UI 退出 guard；这不处理 `TerminateProcess`/任务管理器强制结束、崩溃、关机或 worker 自退，持久 Runtime/WFP 仍是 P0。
- URL/Full Test 只接受精确有界的临时生成配置，`internal-full` 与顶层 `custom_config` 变更会被测试路径拒绝；产品 TUN requested/worker active 任一成立时拒绝新测试，测试运行中也拒绝启 TUN。Full Test 中原先调用系统 DNS 的“入口 IP”查询已禁用，并补上空配置/非法 URL 拒绝、父 RPC context 取消、超时 goroutine 防阻塞和 64 KiB 响应上限；TCP Ping 在 GUI 与 core 两层都明确禁用，因为它使用系统直连 socket。
- 默认文件导出和 `for_share` 别名都使用无产品 TUN/辅助运行态的审计导出；`for_test` 使用独立有界测试配置。所有模式都执行已知 OS 副作用校验，但导出文件仍含凭据且不能视为可任意启动的沙箱。
- 普通 GUI 通过 localhost、每个 GUI session 随机令牌的 gRPC 控制 core；token 在同一会话内沿用，每次 daemon 启动另有不可变 UUID fence，旧协议组合 fail closed。UUID 不是配置授权、持久 generation 或 OS owner。独立 `nekobox_core run/check` 是构建与隔离测试显式使用的高级入口，可直接读取 sing-box 配置；Go 层尚未重复 C++ 的产品策略校验，属于需要补齐的纵深防御边界，而不是普通 GUI 可随意绕过 guard 的证据。
- 无 `-SkipGoBuild`/`-SkipGuiBuild` 的完整打包现在会在同轮 GUI 测试程序与刚构建的 package core 上依次运行 tracker 和 raw QProcess/Qt HTTP/2 Exit，全部通过后才创建正式 zip。任一 Skip 都只产生诊断 package 目录，跳过该 gate 且不创建/覆盖正式 zip。deployment/zip 仍是忽略的本地验收产物，不可交付。

## 2026-07-22 WD/NEX DNS 纠偏与本地迁移

- 权威样本复核：WD 当前 YAML 显式包含 `proxy-server-nameserver`，但其中只有本地 UDP 项；普通 `nameserver` 的公共 DoH 不应被借来解析线路 server。NEX 留存原始 YAML 没有专用字段，普通 `nameserver` 中有三条 HTTPS DoH，因此应由这三条 DoH 解析线路 server。
- 当前忽略目录中的 group 1（WD）已清除旧误判 DoH，记录来源 `proxy-server-nameserver`；group 2（NEX）保留三条 DoH，记录来源 `nameserver`；两组均写入 resolver policy version 1。迁移前原件保存在 `deployment/windows64/config/recovery/manual-dns-policy-migration-20260722/`，不提交 Git。
- 迁移前 group 1/2 备份 SHA-256 分别为 `8D9C9148B8E75B641A73CAE860980F15DA39979E2453DF994C09DCA1AB7F8B6A`、`06194E2EB534176961711DA32B45B24FC209233CD769573B733EB301B388187A`。恢复旧备份后，旧策略非空 resolver 会被新构建器拒绝，必须成功刷新订阅后再运行，不能静默复活错误来源。
- 受控导出验证：WD profile 0 生成 0 个 provider DoH/resolver group；NEX profile 89（含其 WD front proxy）只生成 1 个 strict group、3 个 NEX provider DoH，三条 endpoint 均经 `dns-local` bootstrap，无强制 strategy、无 fallback 字段；两份配置均通过当前 `nekobox_core.exe check`。
- 此迁移和导出没有启动、停止或改写 `D:\Program Files\nekoray`。复核时生产 core PID `11772` 仍持有 `2080`。

## 2026-07-22 无侵入回归快照

- 当前源码的 Windows 全量 C++/package 本地重编译成功。Go core 已通过普通测试、`go test -count=20 ./...`、`go vet ./...` 与 `go test -race ./...`；`grpc_server` 已通过普通测试、vet 与 race。它们证明本轮源码和进程内并发断言，不是父进程/Windows TUN/WFP 生命周期验收。
- 本轮本地审计快照：`nekobox.exe` SHA-256 `3E918885EBB20D0A00FF04FD43E16841E5C0453CCD324C6F5EDE2BB3C3EBB43D`；`nekobox_core.exe` SHA-256 `F545DC44627B83DAF49786F3403ED9E464783D71E6917CE06FDFFC0E147D09E5`；zip SHA-256 `86F3CD775DFF03B13FF6A66DC225FFA1BDDA0B919D504542384C0D743CFBC306`；package RouteFluent manifest SHA-256 `28100CC9F77DE340A3B76A873E476B8EA9D4ECB115B1BA347FFF57345184760A`。当前版本完整 Windows 打包脚本已无 Skip 参数实跑成功：先受保护地清空并重建 GUI build tree，tracker、分享格式、resolver policy 与 raw real-core Exit gate 均 PASS，`deployment/windows64/` 与 zip 已在本地刷新；215 个 package 配置文件恢复完成，两个 preserve 目录和手工诊断产物均无残留。deployment/zip 仍是忽略的本地验收产物，不是 release manifest，也不改变 Alpha/不可发布判断。
- 完整打包/Exit gate 前后的只读生产快照一致：生产 GUI PID `12608`、core PID `11772`，`2080` 仍由 PID `11772` 持有，常见 WinINet 五键不变。脚本未控制这些进程；该快照不证明生产 TUN、路由、DNS 或 WFP 状态。
- `test_final_config_guards.ps1` 15/15（新增原生域名路径、有效/过期订阅组元数据、域名 DoH bootstrap、非法 DoH 与 bootstrap custom 篡改），`test_config_preservation.ps1` 10/10，OpenWrt helper Python 单测 19/19。
- 仓库卫生会解析所有已跟踪 PowerShell，并自测生产路径 exact/subtree/short-name/UNC/device/ADS/SUBST/reparse 等拒绝分支；本轮另以生产 GUI/core 路径参数验证两个测试入口均在启动前失败。这些用例不覆盖 hardlink/final-file identity，不得将其解读为完整的物理文件别名验收。
- 本地 Mixed fixture 7/7；额外 listener、系统代理、禁用日志和 loopback origin 清理均保持预期。
- runtime connectivity 的 expected 204 场景通过，HTTP 与 SOCKS5h 均为 204；expected 200 场景按预期报告 2 项 mismatch 并返回失败。系统代理、fixture 端口和 origin 清理均通过。
- 本轮已在项目 MinGW `bin` 已加入 `PATH` 的环境中执行 `ctest --test-dir build-package-windows64 --output-on-failure`，4/4 通过。`config_recovery` 覆盖配置事务和目录身份边界，`runtime_transition` 覆盖 process-local transition/finished tracker，`share_format_test` 用假凭据覆盖无 remark 及严格凭据列表转换，`resolver_policy_test` 覆盖 WD/NEX 字段选择、DoH URL 校验、domain/IP endpoint bootstrap 与 strict group。CTest 不操作真实 GUI/剪贴板/core。真实 core 的 raw QProcess/Qt HTTP/2 Exit harness 故意不注册到 CTest，只由完整无 Skip package 对刚构建 core 运行；它不调用产品 Client/MainWindow，配置无 listener/TUN，只快照常见 WinINet 五键。上述证据仍不覆盖完整 ProfileManager 刷新事务、DNS 抓取、GUI→Client 或 Windows TUN/路由/DNS/WFP，也不改变 Alpha/不可发布判断。

## 仍然阻断发布

1. external-core/Naive 与误删格式兼容尚未恢复；未知数据已保留并生成 quarantine，结构完整的事务已有显式命令恢复，但图形恢复、未知模型修复仍缺，非空 group 删除因此暂时拒绝。
2. 单文件原子保存、group 创建与部分删除/移动事务已落地，订阅也已先 stage、在 UI 线程串行提交并报告删除失败；订阅成功候选仍未接入单一事务，也缺真实进程终止/磁盘故障和图形恢复验收。
3. 最终 Mixed、strict resolver 与 TUN/系统代理副作用不变量校验已落地，并新增 `test/test_final_config_guards.ps1` 覆盖部分导出拒绝分支；完整 C++ 配置生成 golden/负向回归仍未完成，不能只凭脚本存在判定通过。
4. AnyTLS + Trojan detour 仍失败。
5. 当前 Go wrapper 没有原地热重载；内部 TUN 与单个 Box 同生共死。进程内 lifecycle/generation 已封住直接 Start/Stop/旧引用竞态，但 Windows Runtime Service、stable Mixed/TUN anchor 和 persistent WFP kill-switch 尚不存在；GUI/父进程死亡仍会带走当前数据面。
6. Windows 下主+多辅助真实不同出口、TUN 切线、worker/GUI 崩溃窗口与 IPv4/IPv6/DNS 防泄漏尚未验收；当前 guard 仍违反“开 TUN 时可切线”。
7. 精准系统代理 broker 尚未实现，产品内切换暂禁；旧 WinINet helper 不得重新接回 UI。
8. Go core 仍缺产品策略的第二层校验。process-local lifecycle executor/generation、每 daemon UUID、全 RPC identity validation、协议 v3 ready handshake、服务端 deadline、Start 取消/发布仲裁、command sequence、indeterminate 对账 barrier，以及 Exit 的结构化 ACK/GracefulStop/精确 QProcess finished 已完成；它们可封住 reused port/token、未准入 timeout、部分响应丢失和 GUI-owned daemon 正常退出的已知竞态。但已准入 Stop/Close 仍不可取消，系统也没有持久 RuntimeStateMachine、stable anchor 或 Windows OS 事实源；对账自身超时仍为 unknown，Exit ACK 不确定时只能持续阻断等待。token/UUID/PID 只保护当前调用与实例身份，不解决配置授权或 OS 状态所有权。已有纯状态/race 和安全 raw harness 仍需补 GUI→Client crash→commit/退出、真实 HTTP/2 timeout/ACK 丢失、父进程死亡和 Windows TUN/WFP 集成。
9. 当前提交串行化 mutex 不是完整模型读写锁；ConfigBuilder 两类明确 live-model 写入和 group speedtest worker 读写已止损，但完整 `BuildModelSnapshot`、订阅与其它跨线程读取仍未完成。final Start gate 只复核 recovery、ticket 和已捕获 daemon readiness，不重建 candidate 或比较完整 model revision。route/settings/hotkey 等调用也尚未统一处理保存失败与内存回滚，均属于 Alpha/P0 数据一致性债务。
10. Windows-only CI 已建立并通过；完整无 Skip 打包要求同轮 GUI/core 并通过安全 Exit gate后才写正式 zip，任一 Skip 只产诊断目录。但干净 Qt/MinGW/C++ 依赖工具链、真实交付二进制 manifest 和 Windows 集成验收尚未完成。libneko 已锁定为仓内子模块。

另有一项不冒充当前 P0 发布主阻断的 P2 债务：`TrafficData::last_update` 的未初始化读取已修复，但 counter/rate 仍由 worker 写、UI/JsonStore 无统一同步读取；需后续采用不可变遥测快照或明确锁/原子设计。

本分支不得部署到 `D:\Program Files\nekoray`。后续顺序见 [推进路线](ROADMAP.md)。
