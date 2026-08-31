# 接管状态

状态：现行实现快照；不定义产品需求或后续优先级

审计基线：`agent/takeover-remediation@c7e91f2`

检查点日期：2026-08-31（状态与证据以本文件所在 commit 为准）

> 产品范围只由[产品方向与开发契约](PRODUCT.md)定义；整改顺序和完成门见[已知问题](KNOWN_ISSUES.md)，分层证据见[测试矩阵](testing/TEST_MATRIX.md)。本文件只回答“当前代码实际到了哪里”。

## 结论

当前分支已经纠正了部分严重偏航，并保留了一些有价值的止损机制，但仍是**不可发布的整改分支**。三项核心能力都不是完整交付状态：resolver 只有严格来源选择骨架，人工多入口尚未实现；AnyTLS 有主要数据链和回环运行证据，但生命周期保真与真实远端组合未闭合；专用并发端口的配置路由语义基本正确，但端口选择和启动/重载生命周期仍不满足产品契约。

上一阶段的根本问题不是“防护太多”本身，而是把局部 fail-close 扩张成了全局设计原则：因此删除或禁用了多项上游能力，并把 persistent Runtime/WFP 等候选架构当成发布前提。现行契约已经纠正这一点。后续既不能继续无差别加 guard，也不能把所有 guard 一次性删除；必须按作用域和上游基线逐项证明。

本文的判断分四级，后续维护不得混写：用户最新要求和 `PRODUCT.md` 是产品契约；`adef6cd` 对照及当前调用链是源码事实；带日期/commit 的回环或真实线路记录只是相应环境的运行证据；尚未写出失败回归的静态疑点只能进入整改队列等待复现，不能写成既定缺陷或设计方向。

## 当前仓库与产物

- 当前分支为 `agent/takeover-remediation`，审计起点 HEAD 为 `c7e91f2`；开始本轮时工作树干净。
- 相对上游 `adef6cd`，`c7e91f2` 已变更 231 个文件，约增加 30,678 行、删除 4,970 行；其中包含生产代码、测试、构建和文档，不能把代码量当作完成度或必要性证明。
- 提交历史可分为四层：RouteFluent core/AnyTLS/resolver/辅助端口的扩展；MultiMapper 导出与自动解析实验；external-core/Naive 等上游能力收缩；接管阶段加入的配置恢复、事务、RPC identity/lifecycle 和随后对产品边界的纠偏。四层代码仍同时存在，不能按最新提交时间推断谁是产品权威。
- 高风险集中区不是一个模块：`ConfigBuilder`/`GroupUpdater` 改写数据与配置生成，`mainwindow`/`mainwindow_grpc` 改写上游 UI 和运行生命周期，`ConfigTransaction`/`ConfigRecovery` 建立新的持久恢复协议，Go wrapper 又建立单独 lifecycle executor。后续每个任务必须选一条最短数据链，不能再用一次“整体收敛”同时改四层。
- 受控子模块包括 `third_party/libneko` 和 `third_party/routefluent-sing-box`；后者仍含 RouteFluent 实验字段，普通 GUI 生成路径有 C++ guard。高级 core CLI 可显式读取 raw config，这是独立工具边界；没有产品启动链绕过复现前，不能自动登记成“Go 层必须复制全部 GUI policy”的缺陷。
- 仓库中的 deployment/zip 是被忽略的本机审计产物。最后一次记录完整 Windows package 的二进制早于 2026-07-28 后续整改，不能代表当前 HEAD；本轮只做了增量 GUI 构建，不产生 release 包。

## 本机环境与检查点纪律

- 当前开发主机依赖 Clash TUN 联网。用户已明确冻结为不可中断、不可写入的外部基础网络；本轮没有停止、重启、结束或改写它，也没有修改系统代理、TUN、WFP、路由、网卡或 DNS。
- 当前主机只承担静态、构建、纯逻辑、loopback 和只读证据。用户已取消 Linux/WSL/OpenWrt 后续验证，正式证据只来自 Windows。
- TUN 验收需要本机创建的独立 Windows VM、Windows 沙盒或其它 Windows 测试机。2026-08-31 已启用 Windows Sandbox feature，但 OS 要求重启；为保护 Clash 未自动重启。offline `.wsb`、白名单 staging、runner 和结果 verifier 已准备，等待人工重启后首次运行。具体边界见 [Windows Sandbox 隔离验证](testing/WINDOWS_SANDBOX.md)与 `KNOWN_ISSUES.md` 的 E-001。
- 用户要求后续及时形成远端检查点。每个独立工作包完成验证后应提交并推送当前任务分支，不再跨多轮累积未推送改动；这不授权向 `main` 推送、force push 或合并。

## 三项核心能力

| 能力 | 当前已经成立 | 当前缺口 | 结论 |
|---|---|---|---|
| Clash resolver | `proxy-server-nameserver` present/absent 语义、合法 HTTPS DoH 提取、域名 DoH bootstrap/SNI、strict provider resolver 和禁止跨来源 fallback 已有代码与纯测试 | 没有候选 IP/AAAA/TTL/来源证据/固定入口/解除固定的数据模型和 UI；订阅成功提交仍是多次文件写；尚无 DNS 抓取与泄漏观测 | 正确骨架，不是“多入口已完成” |
| AnyTLS | Bean、UI、链接、Clash 导入、ConfigBuilder、native/Mihomo/custom client 和 group front proxy 已接通；Windows 回环中 AnyTLS + Trojan 前置代理与另一条 HTTP 专用线路并发成功 | 继承来源分享 round-trip、`subscription` query 值、真实远端 AnyTLS + Trojan、显式 chain profile、固定 IP 保留 SNI 尚未闭合 | 可运行骨架，仍缺完整生命周期 |
| 专用并发端口 | 原生 `127.0.0.1:2080` 已恢复普通 NekoRay 路由；辅助 listener 绑定稳定 profile ID 和完整 chain；显式 reject 与精确 terminal 已有纯测试和回环故障隔离 | 首次分配会扫描配置池并在耗尽后随机找空闲端口，UI 没有直接编辑入口；这是尚未冻结的建议/交互问题，不能等同于“已绑定端口冲突后 try-next”。已确认的生命周期问题是：新增映射先保存，再通过整 Box Stop/Start 生效，启动失败可能影响原有 listener 并留下 desired/running 不一致；与内部 TUN 的额外阻止仍待上游审计 | 配置路由语义基本正确；分配体验待决定，变更失败隔离尚未闭合 |

## 2026-08-31 已确认并修正的错误阻止

### `server:port:user:pass` 被误写为仅接受字面 IPv4

该阻止来自提交 `22abd698`。当时文档把目标格式写成 `ip:port:user:pass` 并明确“只接受字面 IPv4”，实现因此使用 `QHostAddress` 拒绝域名和 IPv6；开发者是在执行旧书面约束，不是遇到了 SOCKS5/HTTP 协议本身的限制。旧约束同时混淆了“不为导出调用 DNS”和“server 必须已经是 IP”。

2026-08-31 对 [ipiptest.org](https://ipiptest.org/) 的无真实线路核验显示：其前端历史参数名仍是 `ip:port:username:password`，但提交 `proxy-host.invalid:1080:user:pass` 后服务端进入 hostname DNS lookup，而不是格式拒绝；它在同一入口分别探测 SOCKS5 与 HTTP。带方括号和不带方括号的 IPv6 四字段样本均返回格式错误。结合用户最新确认，本项目的契约名称应为 `server:port:user:pass`。

警告截图中的 `kr-17:` 来自失败列表使用的 `DisplayName()` 前缀，不是导出首字段。成功导出首字段只取 `AbstractBean::serverAddress`，不能取 profile 名称/remark、解析候选或诊断结果。本轮已将代码、菜单和测试统一到 `ServerPortUserPass`：原样保存字面 IPv4、域名和不含分隔符的主机名，不进行 DNS；IPv6 仍因冒号分隔歧义拒绝。四字段格式仅用于带完整认证的 SOCKS5 和非 TLS HTTP；SOCKS4、HTTPS proxy、AnyTLS、Trojan、VLESS 等不能无损映射，继续明确失败。批量操作保持全有或全无。

### TCP Ping

该阻止来自提交 `c0f42da`。上一阶段认为 TCP Ping 使用 `net.DialTimeout`，不经过所选 proxy outbound，因此在 GUI 和 core 两层禁用。这个事实判断正确，但产品结论错误：上游 TCP Ping 本来就是 profile server 经当前 Windows 网络路径的直连可达性诊断，并不等价于 URL Test。

本轮已恢复上游语义并明确标注边界：TCP Ping 不创建临时代理 Box，不声称测试所选 outbound；域名可能由当前系统网络环境解析；活动 TUN 可能影响实际 OS 路径。它只保存诊断时延，不自动切入口或线路。Go 回环测试和 GUI 增量构建已通过，真实 GUI 菜单/远端 server 尚未验收。

### SOCKS base64 userinfo

上游 `adef6cd` 会在 SOCKS URI 没有显式 password 时尝试把 username 当作 base64 `user:password` 解码；提交 `11ba168` 删除 Xray 遗留时一并删掉了这段非 Xray 格式兼容。本轮恢复该入口，但避免上游对“合法 base64、无冒号”输入的错误猜测：只有严格 base64、可逆 UTF-8 且包含冒号时才拆分，首个冒号后的内容全部属于 password；显式 password、非法 base64、无冒号或非法 UTF-8 均保留原值。脱敏规范链接的解析→标准 SOCKS5 导出→重解析纯测试和 GUI 增量构建已通过，真实 GUI 导入/保存仍未执行。

## 上游兼容现状

相对 `adef6cd`，下列回归仍明确存在：

- external-core、Naive、custom external 以及 TUIC/Hysteria2 外核选择被删除或禁用；
- SOCKS base64 userinfo、VMess v2rayN base64 JSON，以及 Shadowsocks legacy/SIP002/v2ray-plugin 基线已恢复；VMess 当前 v2rayN schema 的 `vcn/pcs` 证书验证扩展尚无 Bean/runtime 映射，三类格式的真实 GUI/订阅/线路仍未验收；
- GeoSite 自动完成失去数据源；
- Windows 手工系统代理被整体禁用；
- 在线更新被整体禁用；
- 非空 group 删除、旧 profile reorder 和部分运行中编辑路径被新的全局 guard 阻止；
- 旧 Resolve Domain 因永久覆盖域名而暂停是合理止损，但人工多入口替代功能尚未实现；
- 围绕内部 TUN、退出、切线、辅助端口和 isolated test 的多项阻止源于已被取消的 persistent WFP 前提，尚未逐项对照上游。

其中前五类是明确恢复项；其余三类需要先建立复现和旧数据回归，不能直接回退整套数据/生命周期代码。

## 数据与运行时止损

当前代码已包含 QSaveFile 原子替换、内容寻址备份、未知文件 quarantine、durable before/after 事务记录、磁盘锁、tombstone、RPC daemon UUID、command sequence、generation fencing、Exit ACK 和单 owner executor。这些机制解决了若干真实的数据损坏和跨线程竞态，不能仅因实现复杂就全部移除。

但它们也没有达到完整事务或产品运行时：

- 订阅成功刷新仍由多次 profile/group 保存和删除组成；
- unknown/quarantine 缺少 GUI 恢复；route/settings/hotkey 等保存失败处理不一致；
- ConfigBuilder 和订阅读取尚无完整 immutable model snapshot；
- `TrafficData` counter/rate 仍有跨线程无统一同步访问；
- core 只有整 Box Start/Stop，没有 listener/outbound 原地 reload；
- 高级 `run/check` 明确读取 raw config，不复用 GUI ConfigBuilder policy；这是工具边界而非已复现的产品绕过。普通 GUI Start 是否还需某项 Go 侧窄校验，必须由具体绕过复现决定。

persistent Windows service、stable anchor 和 WFP kill-switch 只是历史候选，不是三项核心能力的发布门。现有 lifecycle 代码只按已证明的竞态收益保留、简化或回退。

## 当前验证边界

截至本轮文档更新前已经重新执行：

- `go test ./...` 与 `go vet ./...`（`go/cmd/nekobox_core`）：通过；包含 TCP Ping 回环监听回归；
- `cmake --build ... --target share_format_test` 与完整本地 CTest 5/5：通过；包含域名/主机名原样导出、不调用 DNS 的纯函数回归；
- `cmake --build build-package-windows64 --target nekobox --parallel 2`：通过；证明当前 C++/UI 可增量编译，不是完整打包。

此前的 5 项 CTest、AnyTLS/Trojan 回环、辅助端口隔离和真实 core schema 证据继续保留在[测试矩阵](testing/TEST_MATRIX.md)，但必须按其日期和 commit 解读。

本轮尚未执行普通 GUI 交互、真实剪贴板、真实域名 TCP Ping、供应商线路、DNS 抓取、TUN、系统代理、WFP、完整 package 或干净 Windows 环境验收，也未修改本机 Clash TUN。

## 发布判断

当前仍不可发布。主要原因是：

1. 上游功能回归未恢复，旧配置与导入分享兼容没有系统对照；
2. 人工多入口管理基本未实现；
3. AnyTLS 生命周期和真实远端组合未闭合；
4. 专用端口的用户选择、冲突和失败隔离生命周期不合格；
5. 数据恢复、订阅原子提交、模型并发和 Windows 集成证据不足；
6. 当前 HEAD 没有同轮完整 Windows package 和 release provenance。

下一步从[已知问题](KNOWN_ISSUES.md)选择单一、可验证的整改切片，不再按[历史路线](ROADMAP.md)的旧阶段继续扩建。
