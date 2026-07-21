# 推进路线

状态：现行
最后更新：2026-07-22

原则：先止损和恢复 NekoRay，再收敛三项扩展，最后解决 Windows fail-closed 运行时。未经需求授权不新增第四项产品功能。

## 阶段 0：冻结边界与保护环境

- [x] 冻结 Windows-only、私人项目、主端口 `12080`。
- [x] 将 `D:\Program Files\nekoray`、`2080` 与生产 TUN 标记为永久 no-touch。
- [x] 冻结“仅手动启停系统代理/TUN、端口精确映射、绝不 fallback直连”。
- [x] 保留上游 `auto_detect_interface=dataStore->spmode_vpn`，未为本机双 TUN 强制开启；测试覆盖默认 preserve。持久 runtime 后改由真实 TUN owner/underlay 拓扑决定。
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
8. [x] URL Test 已恢复为显式有界生成配置；TCP Ping 因系统直连 socket 已在 GUI/core 两层禁用。
9. [ ] 为现用 geosite/geoip `.db` 重建自动完成或明确替代。
10. [x] 订阅改成内存 parse/stage/validate 后提交；空/HTML/坏 YAML/零节点失败旧组零变化；清理/回滚删除失败会保留对象并报告。
11. [ ] 把订阅成功候选与非空 group 删除接入现有事务层，并覆盖真实磁盘失败、并发刷新、进程中断、显式恢复、版本回退和旧 profile。前置止损已完成一部分：联网前按值快照不可变 HTTP 选项，记录 group 与全部成员的身份/顺序/tombstone/序列化状态，提交转回 UI 线程并在提交串行化 mutex 下逐项重验；group 创建同步提交 `pm.json`，删除对象 tombstone，运行中 auxiliary 拒删。下一步必须一次性预检 running/front/chain/remember/活动 auxiliary/未知文件并构造最终新增与删除集合，禁止继续逐个 `AddProfile/Save/DeleteProfile`。
12. [ ] 为 ConfigBuilder、订阅和其它跨线程读模型操作建立完整 immutable snapshot 或显式模型读写同步。当前 mutex 只串行化参与的 mutation 提交，core transition 也只在最终 Start gate 再次串行化和复核，不能宣称整个模型已有读写锁。
13. [ ] 统一 route/settings/hotkey 等保存失败的强类型传播、用户提示和内存回滚；任何磁盘失败后都必须保持磁盘—内存一致，并纳入故障注入。

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

- [x] 只解析 `proxy-server-nameserver`/`proxy_server_nameserver`，区分 absent/valid/invalid三态。
- [x] 使用 provider DoH 的 server-domain outbound 绑定精确 strict resolver，主/辅助绑定均禁止 local fallback；无 provider DoH 的普通节点保留上游正常解析路径。
- [x] 无 provider DoH 时恢复 NekoRay/sing-box普通解析，不使用自定义 local-only group。
- [x] 删除生成配置的普通 nameserver猜测、local fallback/probe，并隐藏 legacy fallback 开关；节点私有 resolver 不再导入。
- [x] 最终 custom merge 校验锁定 outbound、strict resolver group 和 DoH server，并拒绝 RouteFluent fallback/local-only 字段。
- [ ] 决定并实现可审计 bootstrap；当前域名 DoH endpoint 明确构建失败。
- [ ] 增加订阅导入→最终 outbound `domain_resolver` 的 C++ golden tests与断网零本机 DNS测试。

## 阶段 3：Windows 持久 fail-closed 运行时

1. [x] 完成本地 sing-box 生命周期调查：当前无原地 reload，选定持久 service + stable anchor + generation 架构。
2. [ ] 建立单线程 RuntimeStateMachine，分离 desired/observed/owner/health。
3. [ ] 以精确句柄、PID、创建时间、规范路径、config hash和generation管理 worker。
4. [ ] 建立独立于 GUI 生命周期的 Windows Runtime Service/稳定数据面控制点。
5. [ ] 实现独立 persistent WFP kill-switch，覆盖 IPv4、IPv6、DNS和明确例外；不得随 worker/session 消失。
6. [ ] generation执行 validate/prepare → protection active → start/health → commit/rollback。
7. [ ] 系统代理只通过用户手动、按 SID 的 broker操作；关闭时 compare-and-restore完整快照。
8. [x] 过渡期最终配置拒绝任意 sing-box inbound `set_system_proxy=true`、未授权 TUN，并锁定受管 TUN 完整对象及接口策略；同时拒绝已知系统 endpoint/时钟副作用。`internal-full` 与产品 TUN/辅助并发/测试隔离，默认导出与测试导出均走 OS 副作用 guard。
9. [x] UI 区分 TUN requested 与 worker-observed 状态；core 崩溃只重启空控制 core，不自动恢复 profile/TUN。该止损不等于持久 OS 状态或 kill-switch。
10. [ ] 在受控 core/Runtime 入口重复关键产品策略校验，并以 lifecycle mutex/generation 统一保护 `Start`/`Stop`/stats/instance；localhost 随机令牌不替代配置授权。

完成门：GUI退出/重启、worker crash、候选启动失败和切线都不改变系统代理/TUN模式，也没有物理直连；失败可以全阻断。

## 阶段 4：移除越界新增并重建自动化

- [x] 从产品 UI/CMake 移出 MultiMapper专用导出；历史契约留在 archive。
- [x] 移除越界的复杂批量 resolver/change-IP 平台；上游简单 **Resolve domain** 因会走 Windows 系统 DNS 并永久覆盖节点域名，现改为无副作用禁用说明。
- [ ] 如确需恢复 Resolve domain，只能经对应 provider resolver、保留原始域名并提供可验证回退；不得把它重建成探测/改线平台。
- [ ] 把 `test/test_final_config_guards.ps1` 的导出 fixture 下沉为 C++ 配置生成 golden/负向测试，并覆盖 Mixed 完整快照、测试配置边界和产品 TUN 精确对象。
- [ ] 建立 C++配置生成/导入/数据测试和本地 mock outbound矩阵；继续扩展已经落地的 Go nil-config/system-fallback/FullTest 回归到真实 lifecycle 竞争。
- [x] 建立首个 Windows-only CI：校验仓库卫生、固定子模块、受控 RouteFluent core 源构建、Go 单测和无侵入 Python 安全契约；不得将其表述为 GUI/TUN/WFP 验收。
- [ ] 收口干净 Qt/MinGW/C++ 工具链、GUI 自动构建与测试、交付 wrapper 真实 hash/manifest、许可证和 SBOM；libneko 仓内固定已完成。
- [ ] 先用 OpenWrt复测 patched core协议；再在独立 Windows完成 Mixed、多辅助、Wintun、WFP、IPv4/IPv6/DNS故障注入。
- [ ] 只有确实依赖本机生产环境且独立测试无效时，提交维护窗口方案给用户；agent不自行停止生产 NekoRay。

## 阶段 5：私人预览版

- [ ] 所有 P0关闭，旧 NekoRay数据迁移/回退可验证。
- [ ] 三项扩展均有 Windows真实闭环；未验收能力明确标记但不静默删除。
- [ ] 产物版本、hash、manifest和回滚包一致。
- [ ] 内置更新在私人签名源、原子切换和回滚完成前保持禁用。
