# 历史推进路线

状态：**历史执行台账；自 2026-07-27 起不再作为现行需求或开发顺序**
最后更新：2026-07-27

> 本文件保留上一阶段已经做过什么、当时如何判断的证据。后续开发必须重新按 [产品方向与开发契约](PRODUCT.md) 排序，不能根据这里的复选框继续扩建。特别是以下旧结论已经失效：把原生 `2080` 变成无条件主线路入口；把 persistent Runtime/WFP 当作发布前提；把人工多入口管理视为越界功能。MultiMapper 和 RouteFluent 现在是受限参考材料，具体实现仍不可盲目移植。

> 2026-08-31 起，唯一现行整改顺序见[已知问题与整改安排](KNOWN_ISSUES.md)。本文件中的未完成复选框也只表示历史方案当时未完成，不得作为待办领取。下文第 8 项“禁用 TCP Ping”和第 15 项“只允许字面 IPv4 的 `ip:...`”已确认是错误历史结论；现行行为分别见 `PRODUCT.md` 第 3.1 节与第 7 节。

原历史原则：先止损和恢复 NekoRay，再收敛当时理解的三项扩展，最后解决 Windows fail-closed 运行时。该排序和范围判断已被 2026-07-27 产品契约取代。

## 阶段 0：冻结边界与保护环境

- [x] 冻结 Windows-only、私人项目；主端口于 2026-07-24 按 [ADR 0012](architecture/decisions/0012-restore-native-mixed-port.md) 恢复为 `2080`。
- [x] 将本机 Clash TUN 标记为外部底层网络：测试保持其运行，不把其接口/Fake-IP 特例写入产品默认。
- [x] 冻结“仅手动启停系统代理/TUN、端口精确映射、绝不 fallback直连”。
- [x] 保留上游 `auto_detect_interface=dataStore->spmode_vpn`，未为本机双 TUN 强制开启；OpenWrt helper 的既有结果只作历史证据。2026-08-31 后不再安排 Linux L2 重跑，必要 golden 与网络行为改在隔离 Windows 验证。
- [x] 打包脚本移除生产安装默认依赖，运行实例存在时 fail-fast。
- [x] 禁止 Windows legacy 外置 TUN 的占位 PID/按映像名清理路径；未建立精确 owner 前不启动或停止它。
- [x] 删除无令牌的 TUN 提权重启自动启用；旧 WinINet 系统代理切换在精准 broker 完成前从 Windows UI 路径 fail-closed 禁用。
- [x] Windows GUI 忽略 CRT `SIGTERM`/`SIGINT`，关闭绕过 UI 退出 guard 的窄入口；强杀、崩溃、关机仍由阶段 3 解决。
- [x] 建立现行文档、ADR 与历史归档分层。

## 阶段 1：数据止损与上游能力恢复

1. [x] 将单文件配置保存改为检查完整写入、禁止 direct fallback 的 `QSaveFile` 原子替换；现已与多文件操作共享提交串行化 mutex 和跨进程磁盘锁。每次有内容变化的单文件保存会在 commit 前发布短生命周期 durable before/after intent，精确验证 before/after 后退役并尽力删除；`Indeterminate` 保留 `prepared` 并阻断。多文件终态事务作为历史证据持久保留。
2. [x] loader 停止删除损坏/未知/未来 profile/group，并避免复用其 ID；危险 reorder 已禁止。
3. [x] 为单文件覆盖建立内容寻址、写后校验的自动备份；磁盘内容偏离已加载版本时拒绝覆盖。损坏/未知 profile、ID 不一致和已识别的悬空引用会保留原件并生成 snapshot + 审计元数据，GUI 启动时明确提示。
4. [x] 单 profile、空 group 与非活动 route 显式删除前建立内容寻址、回读校验的快照；外部修改、引用关系或快照失败时拒绝删除。旧的非空 group 半删除路径已 fail-closed 禁用，活动 route 必须先显式切换后才能删除。
5. [ ] 建立可选择原件/快照的恢复 UI、显式悬空引用修复和跨 profile/group/主配置的多文件事务。事务基础层已落地：group 创建、单 profile/空 group/非活动 route 删除和 profile 跨组移动会记录 before/after 清单、串行提交、失败逆序回滚，未完成状态会阻断保存和下次启动；启动扫描到完整配置加载也由同一可重入磁盘锁覆盖。扫描会拒绝隐藏/意外条目、身份/header 错误和非终态状态，但终态只做 schema/id/state header 校验；合法 terminal header + 损坏/空 entries 不阻断 startup，report 必须深解析并标为 `valid=false`，非法 terminal schema 仍阻断。命令行已能先报告再由用户明确选择结构化事务的完整 before/after，且恢复开始后锁定方向；它不自动修复 unknown/quarantine。尚未提供图形 UI，也尚未覆盖非空 group 和订阅成功候选。
6. [ ] 选择性恢复 external-core、Naive、ExtraCore、custom external、TUIC/Hysteria2外核；Xray保持删除。
7. [ ] 恢复 VMess/v2rayN、SOCKS userinfo、SS v2ray-plugin等非 Xray 兼容。
8. [x] （历史错误结论，2026-08-31 已纠正）当时因 TCP Ping 使用系统直连 socket而在 GUI/core 两层禁用；现已恢复为与 URL Test 明确区分的 server 可达性诊断。
9. [ ] 为现用 geosite/geoip `.db` 重建自动完成或明确替代。
10. [x] 订阅改成内存 parse/stage/validate 后提交；空/HTML/坏 YAML/零节点失败旧组零变化；清理/回滚删除失败会保留对象并报告。
11. [ ] 把订阅成功候选与非空 group 删除接入现有事务层，并覆盖真实磁盘失败、并发刷新、进程中断、显式恢复、版本回退和旧 profile。前置止损已完成一部分：联网前按值快照不可变 HTTP 选项，记录 group 与全部成员的身份/顺序/tombstone/序列化状态，提交转回 UI 线程并在提交串行化 mutex 下逐项重验；group 创建同步提交 `pm.json`，删除对象 tombstone，运行中 auxiliary 拒删。下一步必须一次性预检 running/front/chain/remember/活动 auxiliary/未知文件并构造最终新增与删除集合，禁止继续逐个 `AddProfile/Save/DeleteProfile`。
12. [ ] 为 ConfigBuilder、订阅和其它跨线程读模型操作建立完整 immutable snapshot 或显式模型读写同步。本批只完成第一段止损：ConfigBuilder 不再回写 live `TrafficData.id/tag`，VLESS core 对象生成不再改写 bean `flow`；group speedtest 改为 UI 线程构建 immutable job，结果回到 UI 后以对象身份、bean/profile/config fingerprint 做 CAS 式复核并保存。当前 mutex 仍只串行化参与的 mutation 提交；final Start gate 只复核 recovery、当前 ticket 和已捕获 daemon readiness，不会重建 candidate 或比较完整 model revision。完整 `BuildModelSnapshot`、订阅与其它跨线程读模型同步均未完成，不能宣称整个模型已有读写锁。
13. [ ] 统一 route/settings/hotkey 等保存失败的强类型传播、用户提示和内存回滚；任何磁盘失败后都必须保持磁盘—内存一致，并纳入故障注入。
14. [ ] （P2）为 `TrafficData` 建立不可变遥测快照或明确的锁/原子方案，使 counter/rate 的 worker 写入与 UI/JsonStore 读取同步。本批仅把未初始化的 `last_update` 固定为 `0`，消除了该字段的未定义行为；generation-local `TrafficBinding` 只隔离路由身份，不解决共享计数器的数据竞态。
15. [x] （历史错误命名/约束，2026-08-31 已纠正）当时实现为只接受字面 IPv4 的 `ip:port:user:pass`；现行契约为直接取 profile 原始 server 的 `server:port:user:pass`，域名/主机名原样导出且不调用 DNS。真实剪贴板交互自动化仍未完成。

完成门：旧配置不会因本分支首次启动或任一失败路径丢失；已有订阅可以安全刷新。

## 阶段 2：把三项扩展收敛为最小实现

### AnyTLS

- [ ] 锁定 native/Mihomo/继承的可逆持久化与链接 round-trip。
- [ ] 非法 client明确拒绝；任意 custom client在验证前退出默认 UI。
- [ ] 定位并修复 AnyTLS + Trojan detour EOF；修复前启动前明确拒绝该已知组合。

### 并发线路

- [x] 主/辅助标准路径使用明确入口→chain映射。
- [x] 空 chain、失效辅助 profile、非法/重复受管端口改为整体构建失败。
- [x] 辅助入口不借主/默认 DNS预先 resolve。
- [x] 主入口同样不在精确绑定前走全局 DNS `resolve`。
- [x] 顶层 custom merge 前快照完整受管 listener 与所有可达 outbound 对象；合并后要求对象逐项相同，并验证 tag/port、精确 terminal rule、outbound type 与 detour 闭环。profile 级 custom outbound 不得改 detour。
- [x] stop/restart/crash/exit 不再清空辅助映射；损坏/重复/错误 JSON 类型使原配置保持不变并中止启动，保存失败时显式操作回滚。
- [ ] 重编规则顺序：保留上游 sniff/reject等不改投动作，同时禁止 direct/其它线路。
- [ ] 让辅助端口 desired/observed 与 runtime generation事务一致。
- [ ] 对暂不能并发的外置 core组合明确拒绝，不删除能力。

### Clash server-domain DoH

- [x] 实现字段存在性驱动的三态：专用 `proxy-server-nameserver` 显式存在时权威且不借普通 nameserver；它 absent 时才提取 `dns.nameserver` 的 HTTPS DoH；两处都无 DoH 时走原生解析。
- [x] 使用 provider DoH 的 server-domain outbound 绑定精确 strict resolver，主/辅助绑定均禁止 local fallback；无 provider DoH 的普通节点保留上游正常解析路径。
- [x] 无 provider DoH 时恢复 NekoRay/sing-box普通解析，不使用自定义 local-only group。
- [x] 删除“专用字段存在时仍猜普通 nameserver”、local fallback/probe，并隐藏 legacy fallback 开关；节点私有 resolver 不再导入。
- [x] 域名 DoH endpoint 使用 NekoRay 原生 `dns-local` bootstrap，保留 SNI、不强制地址族；provider resolver 仍无本机 fallback。
- [x] 最终 custom merge 校验锁定 outbound、strict resolver group、DoH server 和原生 bootstrap，并拒绝 RouteFluent fallback/local-only 字段。
- [x] 增加纯 C++ 订阅来源/DoH 生成策略测试和隔离导出 guard；仍缺完整 ProfileManager 导入→group 持久化→最终 outbound golden 与断网 DNS 泄漏观测测试。

## 阶段 3：Windows 持久 fail-closed 运行时（历史方案，停止实施）

上一阶段曾把下列 RuntimeStateMachine、独立 service、stable anchor 和 persistent WFP 列为发布阻断；该判断已失效。下列清单仅保留历史完成度，不得继续执行。已有 lifecycle executor/generation、daemon UUID 与对账屏障只按实际竞态收益和上游兼容性审计。

1. [x] 完成本地 sing-box 生命周期调查：当前无原地 reload，选定持久 service + stable anchor + generation 架构。
2. [ ] 建立单线程 RuntimeStateMachine，分离 desired/observed/owner/health。
3. [ ] 以精确句柄、PID、创建时间、规范路径、config hash和generation管理 worker。
4. [ ] 建立独立于 GUI 生命周期的 Windows Runtime Service/稳定数据面控制点。
5. [ ] 实现独立 persistent WFP kill-switch，覆盖 IPv4、IPv6、DNS和明确例外；不得随 worker/session 消失。
6. [ ] generation 执行 validate/prepare → protection active → start/health → commit；提交前失败保留当前 generation，提交后失败进入阻断，不自动切回旧线路。只有用户明确选择且旧 generation 重新验证后才允许显式 rollback。
7. [ ] 系统代理只通过用户手动、按 SID 的 broker操作；关闭时 compare-and-restore完整快照。
8. [x] 过渡期最终配置拒绝任意 sing-box inbound `set_system_proxy=true`、未授权 TUN，并锁定受管 TUN 完整对象及接口策略；同时拒绝已知系统 endpoint/时钟副作用。`internal-full` 与产品 TUN/辅助并发/测试隔离，默认导出与测试导出均走 OS 副作用 guard。
9. [x] UI 区分 TUN requested 与 worker-observed 状态；core 崩溃只重启空控制 core，不自动恢复 profile/TUN。该止损不等于持久 OS 状态或 kill-switch。
10. [x] 建立进程内 lifecycle executor/generation 与 daemon identity 基础：GUI 以单一 transition ticket 串行 Start/Stop/CrashCleanup；每次 QProcess 启动生成 UUID，所有 RPC 在 handler 前验证该身份，日志只触发 UUID/协议 v3 握手，旧进程未确认退出时不发布 replacement identity。Start/Stop/Exit 使用单调 command sequence；更高序号的对账与 lifecycle 命令共用 context-aware single-owner executor，并返回目标 command outcome、config hash 和稳定 phase。等待准入的命令可由服务端 deadline 取消；Start candidate 以原子 cancellation-vs-publication 边界决定清理或提交。Go core 使旧/blocked generation fail closed。超时对账成功时只接受精确 active/stopped 结论，再次超时或不一致仍保留 indeterminate。Exit 子项已闭合到进程边界：只在精确 `STOPPED` 返回结构化 `EXITING` ACK，随后 `GracefulStop`；GUI 冻结 generation/UUID/PID 并等待同一 QProcess `NormalExit/0`，不 kill/replacement，ACK 不确定时只有精确 non-admission 对账才恢复控制。详见 ADR 0010/0011。
11. [ ] 在受控 core/Runtime 入口重复关键产品策略校验，并建立可恢复的持久 OS 事实对账。daemon UUID、协议 v3 握手、服务端 deadline、process-local `ReconcileLifecycle`、Start 取消/发布仲裁和 Exit ACK/finished 子项已完成；完整无 Skip package 也会运行 tracker、分享格式、resolver policy 纯测试与安全 raw QProcess/Qt HTTP/2 core gate。但该 raw harness 不调用产品 Client/MainWindow，配置无 listener/TUN，只快照常见 WinINet 五键。已准入 Stop/Close 仍不可中断，对账再次超时仍是 unknown，`GracefulStop` 也可能等待在途 handler；还缺 GUI→Client、crash→commit、真实 timeout/ACK 丢失、父进程死亡和 Windows 路由/DNS/TUN/WFP 资源集成测试。token/UUID/进程内 generation 均不替代持久 runtime transaction、service、stable anchor 或 WFP，因此本项保持未关闭。
12. [x] 删除“先手动关闭 TUN 再继续”的错误 UI 引导。当前仍会阻断会卸载 sing-box-owned TUN 的操作，并明确说明独立 persistent WFP 尚未实现；这只是避免已知直连窗口的止损，不满足“开 TUN 可切线/退出”的完成门。

完成门：GUI退出/重启、worker crash、候选启动失败和切线都不改变系统代理/TUN模式，也没有物理直连；失败可以全阻断。

## 阶段 4：移除越界新增并重建自动化

- [x] 从产品 UI/CMake 移出 MultiMapper专用导出；历史契约留在 archive。
- [x] 移除越界的复杂批量 resolver/change-IP 平台；上游简单 **Resolve domain** 因会走 Windows 系统 DNS 并永久覆盖节点域名，现改为无副作用禁用说明。
- [ ] 如确需恢复 Resolve domain，只能经对应 provider resolver、保留原始域名并提供可验证回退；不得把它重建成探测/改线平台。
- [ ] 把 `test/test_final_config_guards.ps1` 的导出 fixture 下沉为 C++ 配置生成 golden/负向测试，并覆盖 Mixed 完整快照、live/test 的 TUN on/off 四象限、export 删除 `auto_detect_interface` 边界、NTP/嵌套 route direct-action bind 拒绝和产品 TUN 精确对象。
- [ ] 建立 C++配置生成/导入/数据测试和本地 mock outbound矩阵；继续扩展已经落地的 Go nil-config/system-fallback/FullTest 回归到真实 lifecycle 竞争。
- [ ] 批量分享的 C++ 纯函数矩阵已覆盖 fragment 精确删除、IPv4/端口/协议/认证正负例与冒号/换行拒绝，且只使用假凭据；仍需 GUI 自动化覆盖混合多选的原子失败、剪贴板不变和实际右键菜单触发。
- [x] 建立首个 Windows-only CI：校验仓库卫生、固定子模块、受控 RouteFluent core 源构建、Go 单测和无侵入 Python 安全契约；不得将其表述为 GUI/TUN/WFP 验收。
- [ ] 收口干净 Qt/MinGW/C++ 工具链、GUI 自动构建与测试、交付 wrapper 真实 hash/manifest、许可证和 SBOM；libneko 仓内固定已完成。
- [ ] （已被 2026-08-31 用户指令取代）不再执行 OpenWrt/Linux 验证；必要协议与网络矩阵统一在隔离 Windows 完成。
- [ ] （已被 2026-08-31 用户指令取代）当前主机不再安排停用 Clash 的维护窗口；Windows 专属验证转到独立 Windows VM、Windows 沙盒或其它测试机。

## 阶段 5：私人预览版

- [ ] 所有 P0关闭，旧 NekoRay数据迁移/回退可验证。
- [ ] 三项扩展均有 Windows真实闭环；未验收能力明确标记但不静默删除。
- [ ] 产物版本、hash、manifest和回滚包一致。
- [ ] 内置更新在私人签名源、原子切换和回滚完成前保持禁用。
