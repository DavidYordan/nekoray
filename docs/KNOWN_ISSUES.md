# 已知问题

状态：现行发布阻断清单
最后更新：2026-07-22

## P0：范围与数据兼容

### external-core/Naive 被错误删除

提交 `d385a17`、`844d9b2`、`4d68e93`、`96f1166` 删除了 external-core 抽象、Naive、ExtraCore、custom external、TUIC/Hysteria2 外核与生命周期。这超出“删除 Xray”。接管工作树已停止删除无法识别/加载失败的 profile，并防止新 ID 覆盖其原文件；模型与 UI 能力仍未恢复。

修复门：先停止删除和 ID 静默复用；恢复 schema/Bean/UI/导入导出；再把执行能力接回当前架构。暂不支持并发的组合必须明确报错而非丢数据。

### 非 Xray 格式兼容误删

v2rayN VMess 分享格式、SOCKS userinfo、Shadowsocks v2ray-plugin Clash解析和旧分享选项被一并删除。这些是格式/插件能力，不是 Xray 核心。需要选择性恢复并做 round-trip 测试。

### 文件保存和加载仍未形成完整恢复体系

已完成单文件止损：`JsonStore::Save()` 使用禁止 direct-write fallback 的 `QSaveFile` 原子替换，与多文件事务共享参与 mutation 路径的提交串行化 mutex 和跨进程磁盘锁；覆盖已有文件前会把旧内容写入 `recovery/backups/` 的内容寻址备份并回读校验，备份失败即拒绝覆盖。每次实际内容变化还会在 commit 前发布短生命周期 durable before/after intent：精确验证为 before/after 后分别写成 `aborted`/`committed`，移到 `recovery/retired-single-file-transactions/` 并尽力删除；无法判定或无法写终态则保留 `prepared` 并阻断。store 记录加载时“存在/不存在”状态，人工修改或删除目标都不能被旧进程当作新文件重建。已有但无效的关键配置会保留并中止启动；损坏/未知 profile、文件名 ID 不一致和已识别的悬空引用会在 `recovery/quarantine/` 生成原文 snapshot 与原因元数据，GUI 只提示而不自动修复。未知/损坏 profile/group 的 ID 不复用；进程内删除的 ID 也保留为 high-water，危险 legacy reorder 被 fail-closed 禁止。

显式删除单 profile 或空 group 现在也要求磁盘原文与加载快照一致，并先向 `recovery/deletions/` 写入内容寻址、回读校验的删除前快照；被 front proxy/chain 引用的 profile 拒绝删除，删除 remembered profile 会在同一事务清理该引用，core 运行时也拒绝删除当前 auxiliary。group 创建、主配置/group order/删除目标以及 profile 跨组移动涉及的多文件操作，已接入 `recovery/transactions/`：共享提交串行化 mutex 和跨进程锁，先持久化 before/after，再逐项提交；普通失败会逆序回滚。删除对象先 tombstone 再移出 manager，后台测试的迟到 `Save()` 不能复活文件。

未完成状态会禁止当前进程继续保存并阻断下次启动，不会自动猜测恢复方向。启动扫描还会枚举隐藏/系统条目，并对活动锁、意外条目、manifest 缺失/无法解析、schema/id 不匹配和非终态状态失败关闭；精确协议 `.staging-<小写 UUID>` 除外。终态目录在启动路径只校验 JSON 与 schema/id/state header，维护 report 才深解析 operation、entries、snapshot 和当前目标：合法 terminal header 即使 entries 损坏或为空也不阻断 startup，但 report 必须标为 `valid=false`；非法 terminal schema 仍阻断。维护命令可由用户明确选择完整 `before` 或 `after`，方向一旦开始即锁定；它只恢复结构正确的事务，不会自动修复 unknown/quarantine 模型。仍没有图形恢复 UI；目标发生第三种变化、manifest/snapshot 损坏时必须在隔离副本中人工分析。

旧的非空 group 删除会忽略子项失败并继续删 group，现仍整体拒绝；直接放开还会遇到未知文件引用、运行中 auxiliary、测速/编辑窗口旧对象和订阅竞态。订阅清理/回滚会检查删除结果并保留失败对象，但订阅成功候选和非空 group 尚未接入事务层。

路由管理器原先直接删除文件且接受文本框中的路径片段；现只接受安全的 Windows 单文件名，主配置中的非法 `active_routing` 会保持原件并中止加载。非活动 route 使用同一事务层删除；活动 route 必须先由用户加载并确认另一个 route，禁止“先删后切换”。

仍缺：可选择恢复来源和明确提交/回滚方向的 GUI、订阅/非空 group 的事务接入、保存失败的强类型结果、显式悬空引用修复、备份保留/清理策略及真实进程终止/磁盘故障注入。命令行可恢复结构完整且目标未偏离的事务，但这还不是面向普通用户的完整恢复体系。已验证的单文件终态 intent 会自动退役并尽力删除；内容寻址证据、多文件终态事务和删除失败的退役目录仍可能持续增长，尚无统一保留策略。

提交串行化 mutex 不是完整模型读写同步。本批已消除两类明确的构建期 live-model 写入：outbound 统计的 profile id/tag 改为 generation 内按值 `TrafficBinding`，VLESS 的 `-udp443`/`none` flow 规范化改用局部副本，不再回写 bean。group speedtest 也改为在 UI 线程捕获 profile/bean/最终 test config fingerprint 和 RPC 请求，worker 只执行 RPC，结果回 UI 后先以对象身份与 fingerprint 做 CAS 式重验再保存；迟到结果不能写回已经编辑、替换或改变有效配置的 profile。普通保存失败会回滚内存测速结果，不确定保存会保留 intended state 并要求恢复。

这仍不是完整 `BuildModelSnapshot`：ConfigBuilder 尚未在整个读取期持有全模型不可变快照，订阅和其它跨线程读模型路径也未统一。final Start gate 只串行参与 mutation 的路径并复核 recovery、当前 ticket 和已捕获 daemon readiness；它不会重建 candidate 或比较完整 model revision，不能证明构建期间所有读取仍与最终模型一致。部分 route/settings/hotkey 保存调用仍没有一致处理失败或回滚内存。完整不可变快照/读写同步，以及所有设置保存失败时的磁盘—内存一致性，均是 Alpha/P0 债务。

## P0：订阅与 DoH

### 订阅刷新不是事务

接管工作树已恢复刷新并实现 parse/stage-before-mutate：空响应、HTML、坏 YAML、无效 Clash 结构或零支持节点不会创建组或修改旧 group/order/profile。

残余风险在成功候选提交阶段：多个 `AddProfile`、group 保存与旧 profile 删除仍是独立提交，不能覆盖整个刷新；崩溃/磁盘故障仍可能留下新增、重复、残留节点或陈旧引用。当前已让下载/解析并行，联网前按值快照不可变 HTTP 选项，并记录目标 group 及全部成员的身份、顺序、tombstone 和序列化状态；模型提交调度到 UI 线程并用提交串行化 mutex 串行，网络间隔后逐项重验这些快照。新建 group 也会把 `groups/<id>.json` 和 `groups/pm.json` 一次提交。这些前置止损解决容器竞态、ID 并发和 manager 漏写，不等于成功候选已经原子化。

另一个独立边界是订阅下载控制面：继承的默认值 `sub_use_proxy=false` 会由 Qt `QNetworkAccessManager` 直接请求订阅 URL，并使用 Windows 正常解析路径；选择“使用代理”后才经主 Mixed。它不是受管 Mixed 数据面的线路 fallback，也没有由本轮 agent 新增，但确实是一条可直连的控制面路径。若“绝不直连”也覆盖订阅下载，发布前必须改成显式策略（例如强制经已启动主线路，线路不可用即失败），不能悄悄在两者之间回落。

### 历史 DoH 范围已收敛，bootstrap 仍未定义

旧实现曾把普通 `dns.nameserver` 当作 proxy-server resolver、为无 DoH 节点强制创建自定义 local-only group，并提供本机 fallback/公共探测域名。接管已移除生成配置中的 local fallback/probe、隐藏 legacy fallback 开关、无 DoH 节点恢复普通解析，并只接受顶层 `proxy-server-nameserver`/`proxy_server_nameserver`；节点私有 `server-resolver` 不再解析，字段存在但没有有效 HTTPS 项时整次导入失败。仍需：

- 清理仅为旧数据兼容而保留的 fallback/health 字段及第三方 core 中仍可表达旧实验语义的实现；MultiMapper 与复杂批量 resolver/change-IP 平台已移除；上游简单 **Resolve domain** 因使用 Windows 系统 resolver 并永久改写节点域名而暂时禁用；
- 定义 DoH endpoint 域名的 bootstrap。过渡生成器现在拒绝域名 endpoint，只允许 URL host 已是 IP，安全但通常不可用，不能视为需求闭环。

标准生成器与最终 custom validator 已把受管 outbound、strict resolver group、DoH server 锁成完整链，并拒绝 RouteFluent `fallback`/`local_only` 字段。第三方 patched core 的构建补丁和自带文档仍保留该旧实验能力；虽然当前产品配置路径不可达，发布前仍应从受控 core fork 移除或做构建期静态禁止，避免未来回归。

## P0：Mixed、并发线路与最终配置

### 最终受管绑定 guard 已实现，规则语义与自动回归仍不完整

标准生成路径现为 `12080 -> proxy chain`、每个辅助 port -> 对应辅助 chain。空链/失效映射/重复端口已改为构建失败，主/辅助入口都不再在精确绑定前借默认 DNS `resolve`。

标准生成完成、顶层 `custom_config` 合并前，builder 会快照每个受管 Mixed 的完整 listener 以及沿 detour 可达的每个 outbound 完整对象；合并后要求对象逐项相同，并检查 tag/port 唯一、精确无条件 route 绑定、所有 Mixed 的提前 resolve、目标 outbound 类型和 detour 闭环。profile 级 `custom_outbound` 可在快照前修改普通字段，但不得新增/改变 detour。server-domain outbound → strict resolver group → 精确 DoH server 也按生成对象锁定。冲突、缺失、循环、direct/bypass/block/selector/urltest、resolver 篡改和 RouteFluent fallback 字段会构建失败。当前仍缺系统化 C++ golden/负向回归，也需确认规则排序保留 NekoRay 原有 sniff/reject 等非改投语义；不能把这些窄约束表述为任意路由/DNS 配置均已严格证明。

停止、重启、崩溃或退出过去会静默清空辅助端口 map，加载/UI 刷新也会修剪未知/损坏映射。接管工作树已改为仅显式映射操作可修改：字段类型错误、非字符串、损坏或重复项会让既有主配置原件保持不变并中止启动，同时生成可验证 quarantine 证据；显式启停/删除在原子保存失败时回滚内存且不 reload。仍缺可操作的修复 UI 与完整 ConfigBuilder C++ golden。

### 上游路由语义出现回归

把 Mixed 终结绑定放在所有普通规则之前同时绕过了部分 NekoRay block/reject、DNS hijack 与用户路由动作。正确编译顺序应保留不会改投/泄漏的 sniff、reject 等动作，再确保最终只能去绑定线路；不能用简单“所有规则之前”永久替代原语义。

### desired state 与真实 listener 非事务

辅助端口 map 目前可能在 reload 成功前保存；reload因锁、端口或 core 失败后，UI 与旧 listener 可能相反。需要 generation 的 desired/observed 状态与提交/回滚。

## P0：AnyTLS 组合兼容

2026-07-20 的历史 OpenWrt 对照中，AnyTLS（Mihomo client）单跳和相同 Trojan 单跳在三种 Mixed 协议均成功，但二者 detour 组合稳定 EOF。该轮旧探针为所有变体临时强制了 `auto_detect_interface=true`，所以只能支持“同一探针变体下的组合问题”诊断，不能代表产品导出策略；必须按当前默认 preserve 原配置的探针重新运行后再形成正式兼容结论。仍需继续定位 patched sing-box/anytls 的 detour dialer、TLS/ALPN 和 client 语义；在修复前最终 validator 应明确拒绝已知失败组合，不能静默删 detour 或把它报成 Mixed 故障。

AnyTLS client 继承还有保真缺口：显式 native、订阅继承和非法 custom 值可在链接/刷新间混淆。任意 1–128 ASCII custom client 也超出已验证范围。

## P0：TUN 下重启/切线不合格

当前 `Start` 要求旧 instance 为空，`Stop` 会关闭整个 sing-box Box；内部 TUN、Mixed、DNS 和 outbounds 同时消失。UI 目前直接禁止 TUN 状态下的线路/辅助端口变化，并要求用户先关 TUN，这与冻结需求正面冲突。

源码审计已确认当前没有真正的原地 reload：gRPC 的数据面变更仍只有 Start/Stop；`GetDaemonInfo` 只做精确实例握手，`ReconcileLifecycle` 是推进 ordering watermark 的进程内对账屏障，都不是 reload。Stop 会关闭整个 Box；Wintun、路由、DNS 与动态 WFP session 随 worker 消失，core 还会在 GUI 父进程退出后主动结束。标准内部 TUN 现强制 `strict_route=true` 并同时覆盖 IPv4/IPv6，旧 UI 字段只作为兼容数据保留；这只能收紧 worker 活动期，不能覆盖切换、崩溃或 GUI 退出空窗。

本批修复了现有进程内生命周期的直接竞态：GUI 用 generation ticket 串行 Start/Stop/CrashCleanup，替代了“UI 线程加锁、worker 线程解锁”的非法 mutex 所有权；coordinator 在同一临界区同步 participating-mutation depth gate，旧 completion/失败获取不能清除新 owner 或 pending cleanup 的 fence。pending crash cleanup 由当前 transition 连续 handoff，不暴露普通操作可抢占的 idle 窗口，旧 Stop 未明确成功时也不再继续 Start。legacy gRPC `Call` 的完成通知原先还把 `QMutex` 当信号量，由一个线程 lock、另一个线程 unlock；现已改用 `QSemaphore` release/acquire，消除该跨线程同步未定义行为并保证响应写入在等待方可见。Start/Stop/Exit 还携带 GUI session 内单调 command sequence；Go lifecycle 在 phase 检查前推进最高序号，使同一 daemon 中较新的 Stop/Exit 能拒绝后来才抢到锁的旧 Start。启动 candidate 的序列化配置、SHA-256 和受管 TUN 标志在最终 validator 后冻结，成功后不再从可能已变化的 UI 设置反推 worker 状态。daemon UUID/generation 与 profile-request generation 会拒绝旧 crash timer/ready event 操作新进程；queue 与 ready 判断现已原子化，daemon 已 ready 时直接生成 one-shot request，不再在 check→queue 窗口永久搁置。UI 先保留 ready 请求，busy 时等待 transition 排空，只有取得 Start ticket 后才一次消费；显式 Stop/退出、新的直接 user/reload Start 或 daemon stop 都会撤销旧排队/已发未消费请求。Go core 以 lifecycle mutex 串行 candidate 发布、Stop、dial、stats 和 Exit；旧 generation 或 Close 不确定形成的 blocked generation 均 fail closed。lifecycle protocol v2 又把精确 stopped Exit 收敛为结构化 `EXITING` ACK + `GracefulStop`，C++ 以 generation/UUID/PID 跟踪同一 QProcess finished。详见 [ADR 0010](architecture/decisions/0010-process-local-lifecycle-generation-fencing.md) 与 [ADR 0011](architecture/decisions/0011-daemon-identity-and-lifecycle-reconciliation.md)。

这些 fence 全都位于 GUI/core 进程内，没有建立独立 `RuntimeStateMachine`、Windows service、stable anchor 或 persistent WFP。GUI/父进程死亡仍会带走 core 与当前数据面；它们不能证明 TUN 下重启/切线合格。

旧 GUI 把日志文本直接当 ready，而且只检查 stdout；Go `log.Printf` 默认写 stderr，所以真实 GUI 启动可能一直不发送 profile Start，表现为 `12080` 从未监听。现行实现同时读取 stdout/stderr，但日志只作为 `GetDaemonInfo` 探测提示；只有 UUID/协议版本 2 精确回显才发布 ready，并对同一实例去重为一条有界重试链。完整 package 新增的 raw QProcess/Qt HTTP/2 gate 直接验证 core 握手与 Exit 协议，但不调用 GUI 的 `Client`/ready 状态机；因此该控制面修复仍缺真实 GUI→Client 握手证据，也不证明 profile 线路健康。

当前 UI 已把 `spmode_vpn`（用户期望）与 `running_internal_tun`/外置 worker PID（worker 观测）分开显示，并记录成功或不确定 candidate 的 transition generation、QProcess daemon generation、精确 UUID 与 config SHA-256；可明确暴露“requested; inactive”“ACTIVE; stop incomplete”和 indeterminate。受管 TUN observed 值来自最终通过校验的 candidate，而不是 RPC 完成后重新读取 mutable UI。每次 RPC 都冻结并携带目标 daemon UUID；新 daemon 会在 handler 前拒绝旧身份，不再用本地 generation 上界猜测接收者。Start/Stop 应用错误或 transport 失败后，GUI 只向同一 UUID 发出更高 sequence 的 `ReconcileLifecycle` 屏障；只有精确 target outcome、config hash、phase 与 active sequence 一致时才收敛，否则保留 indeterminate。这里的 `ACTIVE`/`STOPPED` 只描述该 daemon 内存中的 `managedCore`，不证明 Windows 接口、路由、Mixed、TUN 或 WFP 事实。Start 已应答但 readiness 在最终 UI commit 前丢失时也显式进入 indeterminate。TrafficLooper binding 只在成功 UI commit 后发布。core 崩溃只重启空控制 core，并保留期望值但不自动恢复 profile/TUN；这避免隐式启用，却会让实际 TUN/保护随 worker 消失，仍是 P0。

`DataStore::core_running` 与 `prepare_exit` 的跨线程访问现已改为 atomic bool；CoreProcess 也不再跨线程读取 `spmode_vpn` 来决定恢复，异常退出统一记录“empty core，不恢复 profile/system proxy/TUN”。退出/重启链会等待同一 UUID 的 Stop completion 或精确 stopped 对账：失败、blocked、superseded、字段不一致或 unknown 时不发送 Exit/quit GUI，而是在 UI 线程撤销 `prepare_exit`/save freeze、恢复 hotkey/control 并保留 observed runtime。Stop 已确定后，协议 v2 Exit 只允许精确 `STOPPED` 原子进入 `EXITING`，返回同一 UUID/sequence/watermark 的 `EXIT/SUCCEEDED` ACK，再由 handler 外 one-shot `GracefulStop` 结束 server；active/blocked 不会隐式 Stop。GUI 发送前冻结 `{QProcess generation, UUID, PID}`，ACK 后按 10 秒报告间隔持续等待同一进程的 `NormalExit/0`，等待期间不 kill/replacement；重复退出由 continuation fence 拦截。ACK 不可用时只在更高序号对账精确证明 `STOPPED/FENCED_NOT_ADMITTED` 后恢复控制，否则即使 server 已不可连接也保持 fence 并继续等 finished，避免把已提交的 Exit 当成失败。Start/Stop 的 30 秒 abort 和 5 秒普通对账仍只界定 GUI 等待，不能取消 Go handler；Exit 的 ACK/对账时限也不是统一 deadline，`GracefulStop` 可能等待在途 handler。因此真正 context-aware 的 single-owner command executor 仍是 P0。tracker 单测覆盖 finished-before/after-wait；完整无 Skip package 还运行安全 raw QProcess/Qt HTTP/2 gate，但它不调用产品 Client/MainWindow、无 listener/TUN，只快照常见 WinINet 五键，不能证明 GUI crash→commit、ACK 丢失、Windows 路由/DNS/TUN/WFP 或生产资源退出。

Windows GUI 已忽略 CRT `SIGTERM`/`SIGINT`，不再让这两个控制台信号调用不安全的 Qt 回调并绕过退出 guard。该修复只关闭一个窄入口：任务管理器/`TerminateProcess`、崩溃、系统关机、父 GUI 消失导致 core 自退和动态 WFP 消失仍未解决，因此不能据此声称退出语义合格。

Go helper 过去在 instance 为空时回落系统 TCP/HTTP，UDP 永远使用系统网络；接管工作树已把这些路径改为明确失败。本批又把普通 core reference/HTTP client 绑定到 generation，禁用跨代 HTTP keep-alive，并在 candidate 清理或 active Close 失败时保留 blocked 状态、拒绝后续 Start/dial/stats/Exit。该补丁只消除进程内 fail-open/竞态，不替代持久保护层。

Windows legacy 外置 TUN launcher 过去只把 `vpn_pid` 设为占位值，停止时按映像名批量 `taskkill nekobox_core.exe`，可能误杀其它安装。接管工作树已禁止该 Windows 启停路径且不删除其配置数据；当前只能使用默认内部 TUN，直到 Runtime Service 能持有精确句柄、PID、路径与 generation。

无令牌的 `-flag_restart_tun_on` 解析、生成和启动时自动启用均已删除。非管理员进程只会提示用户自行以管理员身份重启并再次手动启用，不再把自动连续流程伪装成精准手动操作。

sing-box 的 Mixed/HTTP inbound 原生支持 `set_system_proxy=true`，并会在 listener Start/Close 时自动写入/清理 Windows 代理。最终配置现全局拒绝该字段为 true；受管 TUN 必须与完整生成对象相同，并保留上游 `auto_detect_interface=dataStore->spmode_vpn`，不得出现 `route.default_interface`，也不得由 outbound/endpoint/DNS/NTP 或嵌套 route `action=direct` dialer 覆盖 bind-address/interface。validator 还拒绝系统 WireGuard/Tailscale endpoint 与 NTP 写系统时钟。test 路径同样遵循上游 TUN 意图，文件 export 删除接口自动检测字段；没有本机双 TUN 特例。`internal-full` 在产品 TUN 或辅助并发运行时拒绝、在 latency/full-test 中拒绝；文件导出只有通过同一 OS 副作用 guard 才允许。`test/test_final_config_guards.ps1` 已覆盖其中一部分导出拒绝分支，但仍缺 live/test 的 TUN on/off 四象限、export 删除边界和上述 dialer 路径的完整 C++ golden；并应在受控 core fork 中编译期禁用 Windows 自动系统代理副作用。

没有独立 WFP 过滤层时，简单删除 guard 或“先 check 再 Stop”都不能保证无直连。已选定的 P0 方向是持久 Windows Runtime Service、稳定 TUN/Mixed anchor、独立 persistent WFP kill-switch，以及候选 generation 预检和提交；提交前失败保留当前 generation，提交后故障进入阻断，禁止自动切回旧线路。只有用户明确选择、旧 generation 重新验证后才允许显式回滚。当前每次 core 启动已有独立 UUID，所有 RPC 在 handler 前验证精确身份，ready 也必须通过 UUID/协议 v2 握手；Start/Stop 响应不确定后会用同一 mutex 内的更高 command sequence 对账屏障，成功时可确认精确 active、failed-clean/fenced stopped 或原 runtime 仍 active；Exit 也已有结构化 ACK、ACK 丢失 non-admission 对账和精确 QProcess finished 闭环。它们解决 reused port/token、部分响应丢失和 GUI-owned daemon 正常退出的已知竞态，但 client abort 仍不是可取消的端到端 deadline，对账自身也可能超时；此时 Go handler/屏障可能稍后完成而 GUI 仍必须保留 indeterminate。它们也没有 Windows OS 事实源、Runtime Service、stable anchor 或 persistent WFP，盲目重试仍禁止。

## P0：Windows 系统代理所有权未实现

legacy WinINet helper 不返回可靠结果，也没有保存完整 PAC/proxy/bypass 快照、compare-and-restore、SID owner 或启动后真实状态回读；其 Clear 还会把既有代理设置直接改成 `DIRECT`。接管工作树因此已让 Windows UI 在任何系统代理状态变化前明确拒绝，不调用该 helper。该功能当前不可用但不会误写 OS；只有按 SID 的 broker、原子快照、当前值所有权比较和写后回读完成后才能重新启用。

## P1：NekoRay 功能回归

- GeoSite/GeoIP 自动完成控件仍在，但数据源被清空；需对现用 `.db` 重建读取或明确移除空 UI。
- URL Test 已恢复为通过显式有界临时配置测试所选线路；产品 TUN 已 requested 或内部 worker 活动时都会拒绝新测试，测试未结束时也拒绝启 TUN，避免异步转换窗口把测试流量捕获到错误线路。超时/取消只发请求，直到实际 worker/RPC 退出前仍保持测试活动标记。TCP Ping 因直接打开系统 socket、不能证明所选线路而在 GUI/core 两层禁用。
- sniffing 的旧 destination/routing 模式差异被压成同一动作。
- 上游文档/帮助入口被隐藏；私人分支应指向本地文档，而不是消失。
- MultiMapper 和越界的复杂批量 resolver/change-IP 平台已移出产品代码。上游简单 **Resolve domain** 当前只显示禁用原因，不再解析或保存 profile；若未来恢复，必须通过该节点对应的 provider resolver、不得调用 Windows 系统 DNS，并保留可回退的原始域名。

## P1：构建与测试可信度

- 首个 Windows-only CI 已建立，覆盖仓库卫生、固定子模块、受控 RouteFluent core 源构建、两个 Go 模块普通测试和 OpenWrt verifier 的无侵入安全契约；它尚不构建 GUI，也不验证 TUN/WFP、系统代理 broker 或切线。本地 CTest 仍为 2 项纯测试；其中 `runtime_transition` 还覆盖 `{generation, UUID, PID}` finished tracker 的精确身份、异常完成、重复信号和 finished-before/after-wait，仍不是 Runtime Service 或 Windows TUN/WFP 集成测试。真实 core gate 故意不注册到 CTest，而由完整无 Skip package 在当前 GUI 测试程序与刚构建 core 上依次运行 tracker 和 raw QProcess/Qt HTTP/2 Exit。直接运行测试必须把项目 MinGW `bin` 加入 `PATH`，否则缺失运行库可能以 `0xC0000135` 或 CTest 超时呈现。Go 除 nil-config、system-fallback 和 FullTest 窄回归外，新增 lifecycle candidate 发布/清理、blocked Close、跨代 reference/HTTP client、dial/stats 与 Stop 串行、并发 Start、Exit v2/对账和真实 localhost gRPC ACK→GracefulStop 测试；generation-bound HTTP transport 另覆盖禁用 keep-alive。core 的 count=20/race/vet 与 `grpc_server` 的普通/race/vet 均通过。仍没有 ConfigBuilder/import C++ golden。PowerShell 最终配置 guard 为 10/10；它不是 C++ golden 矩阵，整体覆盖仍有限。
- Go modules 已改为引用固定提交的 `third_party/libneko` 子模块，仓库外源码不再能无 diff 改变产物。该依赖仍公开 system dial API，虽当前无调用且 setup 已 fail-closed 覆盖，发布前仍需静态禁止危险 API并记录依赖许可证/SBOM。
- core 的 `Start`/`Stop`/stats/dial/Exit 现已有 Go 侧 lifecycle mutex 与 generation-bound reference，GUI Start/Stop 也有单一 transition ticket。每代 daemon 的 UUID、协议 v2 握手和全 RPC identity header 封住跨重启误投；session-local command sequence 与对账 barrier 封住 handler 反超，并记录 target outcome/config hash。Exit 另有结构化 `EXITING` ACK、`GracefulStop`、generation/UUID/PID finished tracker 和 continuation fence。这仍只解决当前进程内直接竞态。Start/Stop 的 30 秒 client abort 与 5 秒对账等待都不是服务端可取消的端到端 deadline，Exit 不确定时的无限等待也不是通用 cancellation；系统仍没有 desired/observed/owner/health RuntimeStateMachine、持久 journal、stable anchor、独立 OS 事实源或 GUI/父进程死亡与真实 TUN/WFP 集成测试。
- 普通 GUI 使用 localhost、每个 GUI session 随机令牌的 gRPC，不能无令牌任意调用；token 在该会话内 daemon 重启时沿用，而每次启动的新 UUID 只提供实例 fence，并非配置授权或持久 runtime owner。旧 GUI/core 协议组合会 fail closed。Go `Start` 尚未重复 C++ ConfigBuilder 的 Mixed/TUN/系统代理/resolver 产品策略。`nekobox_core run/check` 又是显式高级 CLI，可直接读取 sing-box 配置并被测试工具使用。两者应准确视为 core 信任边界与纵深防御缺口，不应误写成普通 GUI 功能可任意绕过，也不能把 `check` 成功当成产品策略通过。
- 打包 manifest 由独立 sing-box 构建生成，不能单独证明最终 `nekobox_core.exe` wrapper。`-SkipGoBuild`/`-SkipGuiBuild` 仍可能产生混合版本的诊断 package 目录，所以脚本会跳过真实 core Exit gate，且不会创建或覆盖正式 zip；只有无 Skip 流程才允许生成 archive。
- 干净构建仍依赖仓库外 Qt/MinGW/预构建库；libneko 已锁定为仓内子模块。
- 本机增量构建和 OpenWrt协议测试不能替代 Windows TUN/WFP验收。

当前接管验证已完成一次不带 Skip 参数、先受保护地清空并重建 GUI build tree 的本地完整打包，tracker 与 raw real-core Exit gate 均通过，且没有遗留手工诊断产物。`build-package-windows64/nekobox.exe` 与 `deployment/windows64/nekobox.exe` 的 SHA-256 均为 `DC9DFBEFB06160FBB559B5579C00C2BCFAF625C79AB66486176F78FFB5728384`；core 只输出到 `deployment/windows64/nekobox_core.exe`，SHA-256 为 `ADE5B5EC46CE67E0F9D324B7F543AC82032C0071074B09F9056B90D2E865E59D`，clean build tree 中没有另一份 core。zip SHA-256 为 `6471D4F578A7CFFD0587B5671E38000CC29868148A2BA034A57854B812FF7117`，package RouteFluent manifest SHA-256 为 `028DF56E733869CCFE150544F65D2FA8469795325C17C94ECCAF8E5091AA7C22`。这些值仅是本地审计快照；deployment/zip 被忽略，且尚无独立干净工具链、release manifest 或 Windows 集成验收，不能作为交付证据。

其它无侵入回归为：配置保留/隔离/显式恢复/事务阻断 10/10、OpenWrt helper Python 单测 19/19、本地 Mixed fixture 7/7；runtime connectivity 的 204 正例通过，错误期望 200 被正确拒绝。它们不覆盖真实 Windows TUN/WFP/退出/切线。

已修复：打包不再自动关闭/强杀运行实例，`ReferenceDir` 默认空，空 fallback 路径异常已修复。共享护栏只接受本地固定磁盘盘符路径，拒绝生产根、UNC/设备命名空间、网络映射/可移动盘、SUBST/DOS 重定向、8.3/ADS 与 ReparsePoint/junction，并比对与 `D:` 相同物理卷的盘符别名；持久写入树还会扫描 reparse descendants。它仍未通过最终句柄证明 final-file identity，不能识别所有 hardlink 等同文件别名。陈旧 package preserve 目录会在任何 package 修改前 fail-closed 并保留，只恢复本次运行拥有的备份；完整打包前仍需手动停止目标 deployment 实例，因为脚本会 fail-fast。

## P2：遥测快照与并发一致性

`TrafficData::last_update` 过去未初始化，现已显式设为 `0`，消除了首次速率计算读取不确定值的未定义行为。generation-local `TrafficBinding` 也已把 profile id/outbound tag 从共享对象中移出，避免 ConfigBuilder 改写运行中路由身份。

这不等于统计并发已经解决：`TrafficLooper` worker 仍会写 `downlink`、`uplink`、rate 与 `last_update`，而 UI 的 `DisplaySpeed`/`DisplayTraffic`、Reset 和 JsonStore 路径可无同一锁读取或写入这些字段。当前共享 `TrafficData` 没有完整不可变快照、原子字段集合或统一锁，仍存在真实 C++ 数据竞态与撕裂读风险。后续应在 UI 发布按值遥测快照，或定义覆盖 worker/UI/持久化访问的明确同步协议，并增加 ThreadSanitizer/并发单测；这是 P2 后续项，不得因 `TrafficBinding` 已落地而写成完成。
