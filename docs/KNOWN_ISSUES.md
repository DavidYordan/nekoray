# 已知问题

状态：现行发布阻断清单
最后更新：2026-07-21

## P0：范围与数据兼容

### external-core/Naive 被错误删除

提交 `d385a17`、`844d9b2`、`4d68e93`、`96f1166` 删除了 external-core 抽象、Naive、ExtraCore、custom external、TUIC/Hysteria2 外核与生命周期。这超出“删除 Xray”。接管工作树已停止删除无法识别/加载失败的 profile，并防止新 ID 覆盖其原文件；模型与 UI 能力仍未恢复。

修复门：先停止删除和 ID 静默复用；恢复 schema/Bean/UI/导入导出；再把执行能力接回当前架构。暂不支持并发的组合必须明确报错而非丢数据。

### 非 Xray 格式兼容误删

v2rayN VMess 分享格式、SOCKS userinfo、Shadowsocks v2ray-plugin Clash解析和旧分享选项被一并删除。这些是格式/插件能力，不是 Xray 核心。需要选择性恢复并做 round-trip 测试。

### 文件保存和加载仍未形成完整恢复体系

已完成单文件止损：`JsonStore::Save()` 使用禁止 direct-write fallback 的 `QSaveFile`，检查完整写入和 commit 后才更新内存快照；覆盖已有文件前会把旧内容写入 `recovery/backups/` 的内容寻址备份并回读校验，备份失败即拒绝覆盖。保存前及 commit 前都会比较磁盘内容与本进程加载快照，发现人工或其它进程修改时拒绝覆盖。已有但无效的关键配置会保留并中止启动；损坏/未知 profile、文件名 ID 不一致和已识别的悬空引用会在 `recovery/quarantine/` 生成原文 snapshot 与原因元数据，GUI 只提示而不自动修复。未知/损坏 profile/group 的 ID 不复用；危险 legacy reorder 被 fail-closed 禁止。

仍缺：可选择恢复来源的 UI、删除前恢复副本、跨多个 profile/group/主配置的事务、保存失败的强类型结果、显式悬空引用修复、备份保留/清理策略及完整磁盘故障注入。内容寻址备份可能持续增长，当前绝不自动删除。

## P0：订阅与 DoH

### 订阅刷新不是事务

接管工作树已恢复刷新并实现 parse/stage-before-mutate：空响应、HTML、坏 YAML、无效 Clash 结构或零支持节点不会创建组或修改旧 group/order/profile。

残余风险在成功候选提交阶段：多个 `AddProfile`、group 保存与旧 profile 删除仍是独立文件操作，全局 ProfileManager mutation 也尚未收口到单一串行事务。崩溃/磁盘故障仍可能留下新增、重复或残留节点。

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

AnyTLS(Mihomo client) 单跳和相同 Trojan 单跳在 OpenWrt 三种 Mixed 协议均成功，但二者 detour 组合稳定 EOF。需要继续定位 patched sing-box/anytls 的 detour dialer、TLS/ALPN和 client 语义；在修复前最终 validator 应明确拒绝已知失败组合，不能静默删 detour或把它报成 Mixed 故障。

AnyTLS client 继承还有保真缺口：显式 native、订阅继承和非法 custom 值可在链接/刷新间混淆。任意 1–128 ASCII custom client 也超出已验证范围。

## P0：TUN 下重启/切线不合格

当前 `Start` 要求旧 instance 为空，`Stop` 会关闭整个 sing-box Box；内部 TUN、Mixed、DNS 和 outbounds 同时消失。UI 目前直接禁止 TUN 状态下的线路/辅助端口变化，并要求用户先关 TUN，这与冻结需求正面冲突。

源码审计已确认当前没有真正的原地 reload：gRPC 只有 Start/Stop，Stop 关闭整个 Box；Wintun、路由、DNS 与动态 WFP session 随 worker 消失，core 还会在 GUI 父进程退出后主动结束。标准内部 TUN 现强制 `strict_route=true` 并同时覆盖 IPv4/IPv6，旧 UI 字段只作为兼容数据保留；这只能收紧 worker 活动期，不能覆盖切换、崩溃或 GUI 退出空窗。

当前 UI 已把 `spmode_vpn`（用户期望）与 `running_internal_tun`/外置 worker PID（worker 观测）分开显示：可明确暴露“requested; inactive”和“ACTIVE; stop incomplete”。该观测仍不是 Windows 接口/路由/WFP 的事实源。core 崩溃清理会清除 worker 观测，只重启空控制 core，并保留期望值但不自动恢复 profile/TUN；这避免隐式启用，却会让实际 TUN/保护随 worker 消失，仍是 P0。

Windows GUI 已忽略 CRT `SIGTERM`/`SIGINT`，不再让这两个控制台信号调用不安全的 Qt 回调并绕过退出 guard。该修复只关闭一个窄入口：任务管理器/`TerminateProcess`、崩溃、系统关机、父 GUI 消失导致 core 自退和动态 WFP 消失仍未解决，因此不能据此声称退出语义合格。

Go helper 过去在 instance 为空时回落系统 TCP/HTTP，UDP 永远使用系统网络；接管工作树已把这些路径改为明确失败。该补丁只消除一个 fail-open helper，不替代持久保护层。

Windows legacy 外置 TUN launcher 过去只把 `vpn_pid` 设为占位值，停止时按映像名批量 `taskkill nekobox_core.exe`，可能误杀其它安装。接管工作树已禁止该 Windows 启停路径且不删除其配置数据；当前只能使用默认内部 TUN，直到 Runtime Service 能持有精确句柄、PID、路径与 generation。

无令牌的 `-flag_restart_tun_on` 解析、生成和启动时自动启用均已删除。非管理员进程只会提示用户自行以管理员身份重启并再次手动启用，不再把自动连续流程伪装成精准手动操作。

sing-box 的 Mixed/HTTP inbound 原生支持 `set_system_proxy=true`，并会在 listener Start/Close 时自动写入/清理 Windows 代理。最终配置现全局拒绝该字段为 true；受管 TUN 必须与完整生成对象相同，并保留生成的接口自动检测且没有 default/bind-interface 覆盖。validator 也拒绝系统 WireGuard/Tailscale endpoint 与 NTP 写系统时钟。`internal-full` 在产品 TUN 或辅助并发运行时拒绝、在 latency/full-test 中拒绝；文件导出只有通过同一 OS 副作用 guard 才允许。`test/test_final_config_guards.ps1` 已覆盖其中一部分导出拒绝分支，但仍需 C++ 自动回归，并应在受控 core fork 中编译期禁用 Windows 自动系统代理副作用。

没有独立 WFP 过滤层时，简单删除 guard 或“先 check 再 Stop”都不能保证无直连。已选定的 P0 方向是持久 Windows Runtime Service、稳定 TUN/Mixed anchor、独立 persistent WFP kill-switch，以及候选 generation 预检、提交和回滚。

## P0：Windows 系统代理所有权未实现

legacy WinINet helper 不返回可靠结果，也没有保存完整 PAC/proxy/bypass 快照、compare-and-restore、SID owner 或启动后真实状态回读；其 Clear 还会把既有代理设置直接改成 `DIRECT`。接管工作树因此已让 Windows UI 在任何系统代理状态变化前明确拒绝，不调用该 helper。该功能当前不可用但不会误写 OS；只有按 SID 的 broker、原子快照、当前值所有权比较和写后回读完成后才能重新启用。

## P1：NekoRay 功能回归

- GeoSite/GeoIP 自动完成控件仍在，但数据源被清空；需对现用 `.db` 重建读取或明确移除空 UI。
- URL Test 已恢复为通过显式有界临时配置测试所选线路；产品 TUN 已 requested 或内部 worker 活动时都会拒绝新测试，测试未结束时也拒绝启 TUN，避免异步转换窗口把测试流量捕获到错误线路。超时/取消只发请求，直到实际 worker/RPC 退出前仍保持测试活动标记。TCP Ping 因直接打开系统 socket、不能证明所选线路而在 GUI/core 两层禁用。
- sniffing 的旧 destination/routing 模式差异被压成同一动作。
- 上游文档/帮助入口被隐藏；私人分支应指向本地文档，而不是消失。
- MultiMapper 和越界的复杂批量 resolver/change-IP 平台已移出产品代码。上游简单 **Resolve domain** 当前只显示禁用原因，不再解析或保存 profile；若未来恢复，必须通过该节点对应的 provider resolver、不得调用 Windows 系统 DNS，并保留可回退的原始域名。

## P1：构建与测试可信度

- 首个 Windows-only CI 已建立，覆盖仓库卫生、固定子模块、受控 RouteFluent core 源构建、两个 Go 模块普通测试和 OpenWrt verifier 的无侵入安全契约；它尚不构建 GUI，也不验证 TUN/WFP、系统代理 broker 或切线。CTest 现有 1 项 `config_recovery_test`，覆盖内容寻址备份、幂等、篡改/越界拒绝、隔离元数据和外部修改竞争；仍没有 ConfigBuilder/import C++ golden。Go 已增加 nil-config、system-fallback 和 FullTest 输入/取消/响应上限等窄回归；两个模块普通测试在最终回归通过，本轮较早还用仓库 MinGW、`CGO_ENABLED=1` 实际通过了两个模块的 `go test -race ./...`，但这仍不是 Runtime lifecycle 并发证明。PowerShell 最终配置 guard 为 10/10，覆盖安全 `internal-full`、多类 OS 副作用拒绝、标准 SOCKS profile 的测试/custom/Mixed-detour 拒绝及 custom route 无 `null` 字段回归；它不是 C++ golden 矩阵，整体覆盖仍有限。
- Go modules 已改为引用固定提交的 `third_party/libneko` 子模块，仓库外源码不再能无 diff 改变产物。该依赖仍公开 system dial API，虽当前无调用且 setup 已 fail-closed 覆盖，发布前仍需静态禁止危险 API并记录依赖许可证/SBOM。
- core 的 `Start`/`Stop`/stats 与全局 instance 仍依赖 GUI 串行调用，没有 Go 侧生命周期 mutex/generation；URL/Full Test 的空配置现已显式拒绝，现有 race 测试也未制造完整 GUI/RPC 生命周期竞争，异常并发仍需 RuntimeStateMachine 收口。
- 普通 GUI 使用 localhost、每次启动随机令牌的 gRPC，不能无令牌任意调用；但 Go `Start` 尚未重复 C++ ConfigBuilder 的 Mixed/TUN/系统代理/resolver 产品策略。`nekobox_core run/check` 又是显式高级 CLI，可直接读取 sing-box 配置并被测试工具使用。两者应准确视为 core 信任边界与纵深防御缺口，不应误写成普通 GUI 功能可任意绕过，也不能把 `check` 成功当成产品策略通过。
- 打包 manifest 由独立 sing-box 构建生成，不能证明最终 `nekobox_core.exe` wrapper；`-SkipGoBuild` 还可能混入旧二进制。
- 干净构建仍依赖仓库外 Qt/MinGW/预构建库；libneko 已锁定为仓内子模块。
- 本机增量构建和 OpenWrt协议测试不能替代 Windows TUN/WFP验收。

当前接管验证使用本轮重建的 `build-package-windows64/nekobox.exe` 与 `nekobox_core.exe`。`deployment/windows64/` 仍是 2026-07-18 的旧二进制，未做正式全量打包，不能作为交付证据。

其它无侵入回归为：配置保留/隔离证据 7/7、OpenWrt helper Python 单测 19/19、本地 Mixed fixture 7/7；runtime connectivity 的 204 正例通过，错误期望 200 被正确拒绝。它们不覆盖真实 Windows TUN/WFP/退出/切线。

已修复：打包不再自动关闭/强杀运行实例，`ReferenceDir` 默认空，空 fallback 路径异常已修复。完整打包前仍需手动停止目标 deployment 实例，因为脚本会 fail-fast。
