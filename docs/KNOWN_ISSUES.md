# 已知问题与整改安排

状态：现行整改队列；优先级服从[产品方向与开发契约](PRODUCT.md)

审计基线：`agent/takeover-remediation@c7e91f2`

最后更新：2026-08-31（证据以本文件所在检查点 commit 为准）

> 本文件是唯一现行整改队列。[历史路线](ROADMAP.md)中的复选框、旧 ADR 的 persistent Runtime/WFP 方案和过往 `P0` 命名不再决定开发顺序。每次只领取一个能独立验证的切片；恢复上游与实现新能力尽量分开。

## 优先级定义

- **P0**：当前产品范围内、阻止任何交付候选的缺口，包括明确上游回归、数据丢失风险和三项核心能力的最小闭环。
- **P1**：不阻止继续开发，但稳定版前必须关闭的兼容、生命周期、测试和恢复问题。
- **P2**：已有局部止损、可在核心闭环后处理的并发或工程质量问题。
- **待决定**：会改变用户体验且产品契约没有冻结的内容，只记录到[待确认决策](DECISIONS_NEEDED.md)，不自行实现。

问题证据同时分级：`已确认` 表示已有用户契约、上游 diff、失败回归或可重复运行证据；`源码疑点` 只表示调用链显示有风险，必须先补失败回归；`待实机` 表示纯测试不能替代 Windows/真实线路结论。优先级不提升证据等级。

## 测试环境前置：E-001 隔离 Windows 验证环境

当前开发主机依赖不可中断的 Clash TUN，禁止停止、重启、结束或改写它。后续所有项目 TUN、Wintun、系统代理、Windows 路由/DNS/网卡故障注入和双 TUN 归因都不得在当前主机执行。

整改安排（尚未开始，先由用户审核）：

1. 只读盘点本机可用的 WSL/虚拟化能力，不安装组件、不改变 Hyper-V/vSwitch、路由、DNS、系统代理或 Clash；
2. 把待测断言按平台拆分：WSL 只承接 Linux core、配置、协议和 loopback；OpenWrt 只承接相同 core 的远端链路；Windows 专有断言进入独立 Windows 环境；
3. 优先设计可快照回滚的独立 Windows VM；Windows 沙盒仅在能安装所需驱动且无需重启时作为短生命周期候选。方案须写明 Windows 版本、NAT/隔离网络、测试包哈希、管理员权限、允许创建的进程/接口、凭据隔离和清理复核；
4. 用户确认方案后才创建环境和执行测试。若创建会改变宿主虚拟交换机或网络状态，则停止并另选平台；
5. 测试报告分开标注 `host-loopback`、`WSL/Linux`、`OpenWrt`、`isolated-Windows`，不允许用前三级替代 Windows TUN 结论。

完成门：隔离环境可从干净快照重复创建/回滚；不依赖或改变宿主 Clash；测试只清理自身精确资源；至少能承载 Wintun 创建/销毁和项目 TUN 基本生命周期。E-001 是 Windows 专属验收的前置工作，不是三项产品能力本身，也不授权新增 persistent service/WFP。

## 最近关闭：错误阻止项

| 编号 | 问题 | 处理 | 当前证据 | 尚未验证 |
|---|---|---|---|---|
| G-001 | `server:port:user:pass` 被误写成 `ip:...` 并只接受字面 IPv4，错误拒绝未解析 server | 成功输出只取 profile `serverAddress`，原样保留 IPv4/域名/主机名，不取 `DisplayName()`，不调用 DNS；菜单与函数统一改名 | ipiptest.org 对假 `.invalid` hostname 进入 DNS lookup；`share_format_test` 定向构建与 CTest 通过；`nekobox` 增量构建通过 | 真实 GUI 多选、剪贴板全有或全无 |
| G-002 | TCP Ping 因“不经过所选 outbound”被 GUI/core 双层禁用 | 恢复上游 direct server reachability 语义；明确它走当前 Windows 网络路径，不等价于 URL Test；不再为它构建临时代理配置 | core 回环 listener Go 测试通过；`nekobox` 增量构建通过 | 真实 GUI、域名解析提示、活动 TUN 下结果展示、远端节点 |

上述两项不是放宽 provider resolver 或专用端口的 fail-close。分享导出不产生网络访问；TCP Ping 是用户显式触发的只读诊断，其结果不得自动改入口或线路。

## P0：上游能力恢复

### U-001 导入和分享兼容

已确认相对 `adef6cd` 回归：VMess v2rayN base64、SOCKS base64 userinfo、Shadowsocks v2rayN 格式和 v2ray-plugin 识别。名称包含 `v2ray` 不能作为删除依据。

整改安排：

1. 为每种格式建立脱敏 round-trip fixture，先在当前分支证明失败；
2. 从上游恢复最短 parser/serializer 路径，不恢复 Xray runtime；
3. 覆盖旧链接读取、复制、编辑后再导出和错误输入保留；
4. 每种格式独立提交/验证，不和 external-core 或 AnyTLS 修改混合。

完成门：上游合法样本在当前 sing-box 模型下可无损读取或得到精确“不支持运行”提示，不能静默丢字段或删除 profile。

### U-002 external-core、Naive 与外核选择

Naive Bean/UI/执行、custom external core、TUIC/Hysteria2 外核选择被整体删除。旧 loader 现在会保留 unknown 文件并防止 ID 复用，但这只是止损，不是能力恢复。

整改安排：先恢复 schema/Bean/旧配置读取和编辑，再恢复进程执行与精确 PID/路径所有权；最后验证普通运行。暂不能参与专用并发端口的组合只在该新模式启动前精确拒绝，不得删除 profile。

完成门：`adef6cd` 的脱敏旧配置可见、可编辑、可复制、可导出；普通上游运行路径恢复；Xray 仍不出现在运行 core 选择中。

### U-003 GeoSite 自动完成、在线更新和手工系统代理

- GeoSite 自动完成 UI 仍在，但 reader/数据源被删；
- CheckUpdate 被替换为“私人版本禁用”；
- Windows 手工系统代理因旧 helper 所有权不足被整体禁用。

证据等级：前三项都是相对 `adef6cd` 的源码差异；本轮尚未做真实 UI/Windows 行为复现。整改时把三项拆开：GeoSite 与在线更新先建立最小失败回归，再恢复仍适用于当前数据格式的上游路径；系统代理先在隔离 Windows 环境记录上游启用、禁用、已有 PAC/proxy 三类前后状态，再决定最小兼容修法。现阶段不能把某个 broker、service 或 compare-and-restore 方案预写成既定需求。

完成门：无关上游功能不再无理由消失；系统代理只由用户显式操作，失败保持原 Windows 状态可解释。

## P0：核心能力闭环

### R-001 人工多入口数据模型与 UI（当前基本缺失）

现有 resolver 只保存 group 级 DoH 来源和 strict 生成策略，没有候选 IP、地址族、TTL、发现时间、诊断证据、用户固定入口和 active entry。

整改安排：

1. 先冻结单一权威字段所有权及旧数据默认值；原始 domain、SNI、候选、诊断和固定入口分离；
2. 增加序列化/复制/订阅刷新/删除 round-trip 测试；
3. 实现只读候选刷新和分层诊断，证明不改变 active entry；
4. 实现用户明确固定/解除，固定 IP 连接时保留原 SNI；
5. 最后接 UI，显示 resolver 来源、endpoint、A/AAAA、时间和失败层级。

完成门：`PRODUCT.md` 4.3 六项操作闭环；断开 provider DoH 时无系统/公共 DNS fallback；刷新和诊断不改选择。

### R-002 Resolver 端到端证据与订阅事务

已确认源码事实：标准 GUI 生成路径会拒绝 legacy fallback/local-only 等非产品字段；受控 core 仍能从显式 raw config 表达其中一部分，而高级 `run/check` 是独立 CLI，不等同于普通 GUI 可绕过。订阅刷新虽已 parse/stage/validate，成功提交仍是多次文件写。尚未取得 provider outage 下的 DNS 抓取证据。

整改安排：先补标准 GUI 生成配置的字段矩阵、provider outage 和 DNS 抓取；只有发现产品启动链确能带入非产品字段时，才在最窄入口补第二层校验。不要仅因高级 CLI 可读 raw config 就删除 fork 能力。订阅提交复用现有事务工具，形成 group + order + profiles 的一次原子结果；失败保留旧 group。

完成门：字段矩阵、provider outage、DNS 抓取和提交故障注入通过。

### A-001 AnyTLS 全生命周期保真

源码疑点：`ToShareLink()` 没有序列化 `inheritSubscriptionClient`，因此显式 native 与继承组默认可能生成同样链接；`TryParseLink()` 可把 `anytls_client_mode=subscription` 留在 Bean mode 中，而 `BuildCoreObjSingBox()` 只接受 native/mihomo/custom。上述尚缺专门失败回归。另有带日期的历史证据：真实远端 AnyTLS + Trojan 曾 EOF，而当前 Windows 回环同组合成功；两者只说明需要按同一配置重测，不能宣布协议组合天然不兼容。

整改安排：先补 native/Mihomo/继承三态的 Bean/JSON/链接/Clash round-trip 失败测试，再修序列化顺序和非法值处理；随后按默认 interface preserve 重跑真实单跳、group front proxy、显式 chain 和专用端口。

完成门：新建、保存、编辑、复制、导入、订阅、分享和运行均可逆；组合失败不删除 chain 或静默降级。

### P-001 专用端口分配体验与冲突语义

源码事实：首次建立映射时，UI 扫描配置池，池耗尽后最多随机尝试 32 个当前空闲端口；用户不能直接编辑目标端口。这里还没有“用户已选端口”，因此不能把首次建议直接判定为冲突后的 `try-next`。端口范围、建议方式和 UI 布局本来就在 `PRODUCT.md` 第 13 节待定。

整改安排：用户确认体验前不改分配策略。先建立两个不变量回归：已持久化的端口被占用时必须精确失败且不得换号；首次分配的结果必须在保存前显示并通过 range/duplicate/in-use 检查。是否保留随机建议、是否改为可编辑或仅限配置池，见 `DECISIONS_NEEDED.md`。

完成门：保存与启动前使用同一校验；冲突不写映射、不启动、不换端口。

### P-002 listener 启动失败隔离与可解释状态

新增/删除映射会先在内存中构建候选；候选 chain 构建失败时会回滚且不保存。候选通过后才保存 desired mapping，再用整 Box Stop/Start 生效。端口预检与实际 bind 之间仍有竞态；若 bind 或启动阶段失败，旧 Box 已可能停止，磁盘 desired state 与实际 running state 也可能不一致。

整改安排：先增加“两条已运行 + 第三端口冲突”的失败回归，固定旧 listener 必须继续工作的产品结果；再调查 sing-box 能否在当前 wrapper 内安全 reload。若不能，不得自行引入第二 runtime/service，应提出最小可逆方案和明确限制供用户决定。

完成门：listener 启动失败只影响该变更，既有端口绑定和运行线路保持；状态能区分 desired、validated、running 和 failure。

## P1：过度阻止项专项审计

### 审计原则

每个 guard 必须记录：引入 commit、上游行为、真实风险复现、产品条目、最小替代和回归测试。仅有“可能直连”“更安全”或“当前架构不好处理”不足以永久禁用上游功能。

### 已确认应保留的局部 guard

| 作用域 | 保留原因 |
|---|---|
| provider resolver 禁止本机/公共 DNS fallback | `PRODUCT.md` 3.2、4.1 明确要求 |
| 专用并发端口禁止跨线、direct/bypass fallback | `PRODUCT.md` 3.2、6.2 明确要求 |
| 已持久化端口的范围、重复和占用冲突失败 | 产品要求明确；不得在启动/重载失败后自动换号。首次端口建议方式仍待决定 |
| 未知/损坏数据不覆盖、事务 pending 时阻止 mutation | 防止真实数据损坏，且保留显式恢复路径 |
| 不精确的 legacy Windows 按进程名批量 kill | 可能结束其它安装；只能恢复精确所有权路径 |
| core instance 为空时内部 helper 不回落系统网络 | 防止本应走已选 outbound 的内部调用 fail-open |

### 已确认错误或待恢复

| 编号 | 当前阻止 | 判断 | 下一步 |
|---|---|---|---|
| G-001 | 域名不能导出 `server:port:user:pass` | 已修复 | 补 GUI 剪贴板测试 |
| G-002 | TCP Ping 全局禁用 | 已修复 | 补真实 GUI/网络路径说明 |
| U-002 | external-core/Naive/custom external 全局禁用 | 明确上游回归 | 分层恢复数据模型与普通运行 |
| U-003 | GeoSite 自动完成、在线更新和手工系统代理被禁用/删除 | 源码差异已确认，实际行为待分项复现 | 分开建立回归，不预设新 broker/service 架构 |
| U-004 | profile reorder、非空 group 删除 | 功能回归但旧实现确有半删除风险 | 复用现有事务做局部安全实现 |
| U-005 | internal TUN 活动时禁止切线、删除、辅助端口变更、退出 | 基于已取消的 persistent WFP 产品前提，是否全部错误尚待逐项复现 | 对照 `adef6cd` 建四象限测试后逐条移除或缩窄 |
| U-006 | TUN requested/active 时禁止 URL/Full Test，`internal-full` 多处拒绝 | 可能有测错线路/OS 副作用风险，但当前限制面大于产品契约 | 区分“结果标签不准确”和“真实 OS 副作用”，建立最小测试后决定 |
| U-007 | 旧 Resolve Domain 全局禁用 | 暂停永久改写域名是合理止损，但不能长期替代 R-001 | 不恢复破坏性旧动作，完成新人工多入口闭环 |

禁止通过一个“大撤 guard”提交处理 U-004 至 U-007；每项都必须有独立前后证据。

## P1：数据与恢复

### D-001 订阅/批量操作原子提交

单文件保存、部分创建/删除/移动已有 durable intent 和回滚，但订阅、非空 group 删除、批量 reorder 尚未形成一次可恢复提交。优先复用现有 `ConfigTransaction`，避免再建第二事务框架。

### D-002 GUI 恢复与未知数据可见性

unknown/quarantine 文件被保留且 ID 不复用，但用户在 GUI 中看不到内容和恢复动作。应提供只读报告、导出原件和用户明确恢复/删除入口；不得自动猜测类型。

### D-003 保存失败一致性

route/settings/hotkey 和部分 UI 状态仍可能在磁盘保存失败后保留新的内存值。逐路径建立失败注入，要求内存回滚或明确 indeterminate，不做全局重写。

## P2：并发与工程质量

- **C-001 TrafficData**：counter/rate/`last_update` 在 worker、UI、Reset 和 JsonStore 之间无统一同步；采用按值快照或明确锁协议并补并发测试。
- **C-002 BuildModelSnapshot**：ConfigBuilder 与订阅已有局部 CAS/冻结，但没有完整模型 revision；先收集真实竞态再决定最小同步范围。
- **C-003 core 边界复核**：先证明普通 GUI Start 存在可绕过 C++ 最终配置 guard 的路径；只有该复现成立，才在 Go 侧重复对应的窄不变量。高级 CLI 保持显式 raw-config 工具边界，不扩张成全局策略平台。
- **C-004 构建 provenance**：建立同一版本源、当前源码的完整无 Skip package、二进制 manifest、许可证/SBOM 和干净 Windows 重建。

## 待用户审核的下一阶段工作包

以下只是安排，不代表已获准开始；每个工作包单独验证、提交并推送当前任务分支，不跨包混改：

| 顺序 | 工作包 | 目标与证据 | 明确不做 |
|---|---|---|---|
| C0 | 当前审计检查点 | 固化 G-001/G-002、产品/文档纠偏、Clash no-touch 约束和当前验证结果，并推送远端任务分支 | 不执行 GUI/TUN/系统网络测试 |
| W1（建议下一项） | U-001a：SOCKS base64 userinfo 兼容 | 先用 `adef6cd` 脱敏样本证明当前失败，再只恢复 SOCKS parser/serializer 的最短 round-trip；纯本地测试 | 不同时恢复 VMess、Shadowsocks、external-core，不访问网络 |
| W2 | U-001b/c：VMess v2rayN 与 Shadowsocks/v2ray-plugin | 两种格式各自建立失败 fixture、字段保真和错误输入测试，各自独立提交 | 不因名称含 `v2ray` 恢复 Xray runtime |
| W3 | R-001a：人工多入口字段契约 | 只冻结原始 domain、SNI、resolver 来源、候选、诊断、固定入口的单一所有权和旧数据行为，先写 round-trip 测试 | 不做自动择优、DNS fallback 或 UI 大改 |
| W4 | E-001：隔离环境方案 | 只读能力盘点并形成 Windows VM/沙盒方案；用户二次审核后才创建 | 不停止/改写 Clash，不改变宿主网络，不用 WSL 冒充 Windows TUN |
| W5 | 逐项恢复 U-002/U-003 | external-core/Naive/schema 与普通上游能力按数据→UI→执行分层恢复；GeoSite、更新、系统代理拆成独立工作包 | 不与 AnyTLS/resolver/端口生命周期混改 |
| W6 | 三项核心闭环 | 依次推进 R-001/R-002、A-001、P-001/P-002；涉及 Windows TUN 的 U-005/U-006 必须等 E-001 可用 | 不引入第二 runtime、persistent service/WFP 或全局 fallback |
| W7 | 数据、并发与交付 | D-001..D-003、C-001/C-002，最后做同轮完整 package、旧配置迁移、真实 GUI/线路和分层网络矩阵 | 不用旧二进制或低层测试代替交付证据 |

推荐先执行 W1：它与本轮分享格式审计同域、改动面最小、可完全离线验证，也不会受本机 Clash/TUN 环境影响。W4 可在后续作为独立的只读规划包推进，但隔离环境创建和任何 Windows 网络测试必须再次经过用户审核。

任何步骤若需要自动择优、全局 fallback、persistent service/WFP、第二数据模型或不可逆迁移，必须停止并请求用户决定。
