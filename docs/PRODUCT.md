# NekoRay 私人分支：产品方向与开发契约

状态：**现行，后续开发的唯一产品范围源**
最后更新：2026-08-31
上游比较基线：NekoRay 4.0.1，commit `adef6cd`

## 0. 文档权威性

本文件用于防止接管开发再次偏离原始目的。发生冲突时，按以下顺序解释：

1. 用户最新的明确要求；
2. 本文件；
3. 当前代码、测试和运行证据；
4. `TAKEOVER_STATUS.md`、`KNOWN_ISSUES.md` 与实施路线；
5. ADR、历史审计和 `archive/`。

代码“已经这样实现”、测试“已经按它编写”或旧 ADR“曾经批准”，都不能反向创造产品需求。RouteFluent、MultiMapper 以及当前分支都只是调查材料，不是需求本身。

## 1. 起点与产品定位

本项目起源于三个彼此相关但必须分清的能力：

1. **Clash 订阅的 server-domain 解析与多入口管理**：同一线路域名可能有多个入口；订阅自带 DoH 往往能看到专线入口，公网 DNS 往往能看到常规跨境入口，但这只是常见现象，不是永真优先级。用户必须能看清来源并手工决定。
2. **AnyTLS 支持**：Xray 已明确不会支持 AnyTLS，因此运行时只使用 sing-box 系路线，避免误选 Xray。
3. **多线路并发端口**：多条线路需要同时通过不同本地端口提供服务，每个专用端口精确对应一条完整线路。

批量分享格式等属于少量、边界清楚的便利功能。

本项目是对稳定 NekoRay 4.0.1 的**最小补强**，不是重新设计代理客户端，不以“智能路由”、自动择优或全新运行时平台为目标。任何改动都应先回答：

- 它直接服务哪一项已确认需求？
- 为什么不能复用或窄改上游能力？
- 会改变哪些上游行为、数据或用户操作？
- 如何证明没有让无关功能退化？

答不清时，默认不改。

当前首要验收平台是 Windows x64 私人便携构建。这不自动授权删除上游非 Windows 代码；平台裁剪也必须有真实冲突和明确收益。

## 2. 术语：不要再混淆入口与线路

| 术语 | 本项目中的含义 |
|---|---|
| 线路 / profile | 一套完整出站身份及其 front proxy、detour、resolver 和协议参数 |
| server domain | profile 中原始服务器域名；它也是 TLS SNI/证书语义的一部分，不能因解析而丢失 |
| 网络入口 / entry | 某 resolver 视角下，server domain 得到的一个候选 IP；不是本地监听端口 |
| resolver 来源 | 订阅专用 DoH、订阅普通 nameserver 中的 DoH，或 NekoRay 原生解析路径 |
| 多入口管理 | 保存候选入口的来源与证据，并由用户手工固定或解除固定 |
| 并发线路端口 | 本机上的专用 SOCKS/HTTP/Mixed listener；每个 listener 精确绑定一条逻辑线路 |
| fallback | 当前指定来源或线路失败后，程序自行改用另一来源、线路或 direct |
| 诊断 | 采集 DNS、TCP、TLS、协议、延迟等事实；诊断不得自行改变选择 |

“一个域名有多个网络入口”和“多条线路各有一个本地端口”是两层不同问题。两者可以组合，但数据模型和失败边界不能混为一体。

## 3. 全局硬约束

### 3.1 手工控制，不做智能路由

- 不根据延迟、地理位置、晚高峰表现、单次失败或所谓健康评分自动换入口、换线路或换 resolver 类别。
- 诊断只提供证据；最终选择由用户明确操作。
- TCP Ping 保留为上游的 server 直连可达性诊断：它通过当前操作系统网络路径连接 profile 的 server，域名可能由当前系统网络环境解析；它不代表所选代理 outbound 的 URL Test。UI 和文档必须明确两者差异，结果只作为诊断证据，不得自动改入口或线路。
- 专线入口并不恒定优于公网入口，公网入口也不能被永久视为降级项。
- 自动回退、自动轮询、自动负载均衡和“最佳 IP”不是默认行为。

### 3.2 失败关闭，但不夸大范围

- 已绑定 provider DoH 的 server-domain 解析失败时，不得静默借用本机/公网 DNS。
- 专用并发端口绑定的线路失败时，只允许该端口失败；不得改投主线、其它线路、`direct` 或 `bypass`。
- 一个候选 IP、resolver、运营商入口或协议探测失败，不代表整个 profile 已失效，更不能触发自动改线。
- “失败关闭”针对本项目明确受管的解析和专用端口，不授权构建全机 WFP、持久 service 或全新网络控制平面。

### 3.3 保留上游

NekoRay 4.0.1 的协议、profile/group 数据、导入导出、分享、路由、测速、系统代理/TUN 手工操作、front proxy、外置 core、Naive、插件兼容和 UI 行为默认保留。

唯一明确删除的是：

- Xray **运行核心及真正只服务于该运行核心的路径**。

以下名称或能力不等于 Xray，不能据此删除：

- `v2rayN` 分享格式；
- `v2ray-plugin`；
- Qv2ray/NekoRay 兼容组件；
- VMess/VLESS 等由 sing-box 或外置 core 承载的协议；
- 名称中含 `v2ray` 的数据结构、文件或辅助函数。

旧能力与新需求真实冲突时，先给出复现、兼容方案和数据迁移方案，再做最小修改。暂不能支持某一组合时，保留数据并给出精确错误。

### 3.4 本机基础网络与 TUN 验证边界

- 当前开发主机依赖 Clash TUN 联网。它是不属于本项目、不可中断的外部基础网络，必须始终保持运行。
- 本项目、agent、构建和测试不得停止、重启、结束或接管 Clash 进程，也不得改写其配置、接口、Fake-IP、DNS、路由或其它 Windows 网络状态。不能把“申请维护窗口停掉 Clash”当作补证据方案。
- 当前主机只执行不改变网络状态的静态检查、构建、纯逻辑、loopback 和只读快照。若项目 TUN 与 Clash 的叠加使结果无法归因，应明确记录环境限制并转移验证，不得把本机特例写入产品默认。
- WSL 可用于 Linux core、配置生成、协议和隔离 loopback 证据，但不能证明 Windows Wintun、系统代理、WFP、驱动、GUI 生命周期或双 TUN 行为。OpenWrt 证据有同样的 Windows 边界。
- 需要启停本项目 TUN、注入网卡/路由/DNS 故障或验证 Windows 专属行为时，应使用独立 Windows VM、Windows 沙盒或其它与本机 Clash 解耦的 Windows 测试环境，并分别记录快照、网络拓扑、进程所有权和恢复结果。
- 没有合格隔离环境时，相应层级保持“未验证”；低层 fixture、WSL、OpenWrt 或本机 Clash 叠加环境的成功不能替代 Windows TUN 验收。

## 4. 核心能力一：订阅 resolver 与多入口管理

### 4.1 Clash 字段读取

Clash YAML 按字段存在性处理，不用猜测：

1. `dns.proxy-server-nameserver` 或 `dns.proxy_server_nameserver` **显式存在**时，它是 server-domain resolver 的权威来源；只提取其中合法的 HTTPS DoH。即使列表为空或只含非 HTTPS 项，也不得转而借用普通 `dns.nameserver`。
2. 上述专用字段**完全缺失**时，才从 `dns.nameserver` 提取合法 HTTPS DoH。
3. 两处都没有可用 HTTPS DoH 时，该订阅不获得 provider resolver；节点保持 NekoRay/sing-box 原生解析语义，UI 不得伪装成“严格 DoH”。
4. 被选来源中的 HTTPS DoH 语法错误必须使整次订阅刷新失败，旧 group 保持不变；非 HTTPS 项可忽略，但要形成无敏感信息的可见统计。

同一个显式 provider resolver 集合中，可以按确定顺序尝试多个 DoH endpoint；这属于同一来源内部的 endpoint 冗余，不得跨到公共/本机 DNS。实际使用了哪个 endpoint 应可诊断。

DoH endpoint 自身为域名时，可以由 NekoRay 原生 bootstrap resolver 建立 DoH 传输并保留 HTTPS SNI。bootstrap DoH 主机和“用本机 DNS 解析线路 server”是两件事，后者仍禁止。

### 4.2 多入口数据必须保留的事实

解析或诊断不得直接把 profile 的域名永久改成 IP。至少保留：

- 原始 server domain；
- 订阅/group 身份与 resolver 来源；
- resolver endpoint 身份；
- 每个候选 IP、地址族、发现时间和可取得时的 TTL；
- DNS、TCP、TLS、协议诊断结果及其时间；
- 用户当前固定的入口；
- 原始 TLS SNI/证书主机语义。

延迟、地理位置、运营商和可用性只是证据。不得把一次 TCP 443 connect、第三方 GeoIP 结果或最低延迟直接写成 active entry。

### 4.3 用户操作

最小可用闭环包括：

1. 按 profile 查看原始域名、resolver 来源和全部候选入口；
2. 明确刷新某个来源的候选，刷新本身不改变 active entry；
3. 手工固定一个入口，运行时连接该 IP 但保留原始 SNI；
4. 手工解除固定，恢复该 profile 原本的 resolver 行为；
5. 分别诊断候选入口，并展示失败发生在 DNS、TCP、TLS 还是协议层；
6. 任何变更都有可见的 before/after，保存失败时旧选择不变。

可以提供“命名入口集合”用于整理候选。若集合包含多个 IP，在用户另行确认运行策略前，它只是一种管理数据；不得默认引入轮询、哈希分配、自动最佳 IP 或失败切换。

### 4.4 明确禁止

- provider DoH 失败后自动改用 Windows、路由器或公共 DNS；
- 根据夜间拥塞、单次延迟或单个入口失败自动切换 profile；
- 将解析结果永久覆盖原始域名；
- 让诊断按钮顺便修改线路；
- 把 MultiMapper 的 `best_ip`、自动 round-robin pool 或外部 GeoIP 服务原样移植。

## 5. 核心能力二：AnyTLS

- 支持新建、保存、编辑、复制、导入、订阅刷新、分享和真实运行 AnyTLS profile。
- 字段必须可逆持久化，并显式区分 native、Mihomo compatibility 和实际继承来源；不能仅凭“来自 Clash”永久猜测 client。
- AnyTLS 必须参与 NekoRay 原有的 group、front proxy、detour、路由、并发端口和 resolver 体系。
- AnyTLS + Trojan/front proxy 等组合失败应先作为兼容 bug 调查，不能据此删除 NekoRay 的 chain 能力。
- 使用支持所需特性的 sing-box fork 可以是实现手段，但不能把 RouteFluent 的 OpenWrt 产品模型一并搬入。
- Xray 不进入运行时和 UI 可选 core；非 Xray 的旧格式与外置 core 仍保留。

## 6. 核心能力三：多线路并发端口

### 6.1 产品语义

多线路并发的目的，是让多个 profile 同时通过不同本地端口被显式使用。每个专用 listener 必须持久关联：

`listener identity -> listen address/port -> profile identity -> 完整 outbound chain`

完整 chain 包括该 profile 的 front proxy/detour、resolver、AnyTLS client 类型和必要的出站对象。不能只绑定最后一跳 tag。

### 6.2 原生主入口与专用线路入口

- 原生 `127.0.0.1:2080` 继续承担 NekoRay 的正常 Mixed 入口语义：当前选中 profile 是默认出站，但用户原有路由、阻断、DNS 和协议处理不能被一条新加的无条件规则整体遮蔽。
- 新增的**专用线路端口**才是严格的线路选择器。其代理流量只能进入所绑定的完整 chain；显式 reject/block 仍可生效，但不得静默落到 direct、主线或其它线路。
- 如果未来需要“严格主线路专用端口”，应创建一个明确的专用 listener 或显式模式，不能悄悄重新定义原生 `2080`。

这一区分是“最小补强”与“推翻 NekoRay 路由模型”的边界。

### 6.3 端口与生命周期

- 端口映射按稳定 profile/listener 身份保存，不能依赖列表序号推导。
- 保存/启动前检查端口范围、重复、监听地址和实际占用；冲突必须报错。
- 运行时不得遇到占用就“试下一个端口”，不得静默重排、删除或重绑。
- 默认只监听 loopback；Allow LAN、认证和暴露范围沿用上游能力，任何扩大暴露都需用户明确操作。
- 一个专用 listener 启动失败，不得改变其它已运行 listener 的绑定。
- stop/restart/crash 不得清空用户保存的映射；配置切换失败时保留最后明确、可解释的状态。
- 外置 core 或高级组合暂不能进入并发托管时，只拒绝该组合并说明原因，不删除 profile 或旧能力。

端口池的具体默认范围属于实现参数，不是产品原则；更不能为了保留某个历史常量而扩大架构。

### 6.4 不授权的扩展

多线路并发不等于：

- 自动负载均衡或故障转移；
- 按延迟选线；
- OpenWrt Segment/DHCP/TProxy/nftables 模型；
- 独立 Windows service、WFP kill-switch 或持久数据面；
- 改写 NekoRay 所有运行、导出和测试入口。

## 7. 已明确的便利功能

保留上游分享功能，并支持：

- 多选复制原生链接；
- 只移除 URI fragment/remark 的原生链接；
- 对可无歧义表达的 SOCKS5/非 TLS HTTP profile 导出 `server:port:user:pass` 四字段文本。`server` 必须直接取 profile 的原始 `serverAddress`，不能取 profile 名称/remark，也不能替换成诊断得到的候选 IP；它可为字面 IPv4、域名或不含分隔符的主机名，导出不得调用 DNS。IPv6 因冒号与字段分隔符冲突，在没有另行确认转义规则前明确拒绝。

批量转换全有或全无；失败不改变剪贴板。server 为空或包含冒号/空白字符时拒绝；username/password 缺失或包含冒号/换行时拒绝。不得为分享调用 DNS，也不得把凭据写入日志、测试快照或错误详情。

其它便利功能必须同样满足：需求明确、实现局部、不会改变线路运行语义。

## 8. 两个参考项目应如何使用

### 8.1 RouteFluent：借严格不变量，不借产品架构

可借鉴：

- listener/线路/完整 chain 的稳定显式关联；
- 端口持久分配、唯一性与占用预检；
- 不“try next”、不自动 direct、不跨线路 fallback；
- 诊断不修改选择；
- 一条线路失败只影响自己的入口。

不可照搬：

- OpenWrt Segment、DHCP、TProxy、nftables 和网关部署模型；
- “绿地项目可删除兼容层”的前提；
- 用持久网络控制平面重构成熟 NekoRay。

### 8.2 MultiMapper：借多入口语义，不盲信实现

可借鉴：

- 把原始域名、来源 tag、resolver、候选 IP、固定 IP 和诊断证据分开；
- 按来源比较入口，不把单一入口失败等同于整条线路失败；
- 用户手工固定/解除入口；
- 多路由各自拥有明确 listener 和 chain；
- 变更前预检、失败保留旧状态。

不可直接移植：

- 以 TCP 443 延迟自动生成 `best_ip`；
- 多 IP pool 自动稳定轮转；
- provider DoH 失败后的本机/公共 DNS fallback；
- OpenWrt rewrite/deploy、LAN Segment 和生产环境专用库存；
- 第三方 GeoIP 结果驱动运行选择；
- MultiMapper 当前字段、UI 或 resolver builder 作为 NekoRay schema。

MultiMapper 是过渡性探索项目。它证明了问题真实存在，也提供了有价值的操作语义，但不能证明其实现适合 NekoRay。

### 8.3 本轮调查坐标

为便于后续复核，本轮参考的是：

- RouteFluent `D:\complex\RouteFluent`，`main@df220efbfe98`：
  - `AGENTS.md`
  - `docs/DOMAIN_MODEL.md`
  - `docs/RUNTIME_OPENWRT.md`
- MultiMapper `D:\python\MultiMapper`，`agent/legacy-initialize-lan-20260727@5adab3116abf`：
  - `README.md`
  - `docs/HANDOFF_FOR_DEVELOPERS_2026-07-14.md`
  - `docs/NEX_WD_ENTRY_OPERATIONS.md`
  - `docs/NEKORAY_COMPACT_YAML_IMPORT_CONTRACT_2026-07-18.md`
  - `docs/PRODUCTION_MAINTENANCE_BOUNDARIES.md`
  - `routing_tab.py`
  - `singbox_config.py`
  - `server_dns.py`

外部仓库会继续变化；上述 commit 只用于重现本轮理解。以后参考新版本时，先重新检查其产品前提和自动策略，不能只看最新代码。

## 9. 对当前分支改动的重新分类

以下是接管调查后的工程分类，不把“代码很多”简单等同于好或坏。

### A. 方向正确，应保留并收敛

- Clash DoH 字段存在性语义；
- provider server-domain resolver 禁止本机 DNS fallback；
- AnyTLS profile/import/share/core 支持骨架；
- 批量分享的局部实现；
- 专用辅助 listener 精确绑定完整线路的思路；
- 订阅 parse/stage/validate 后提交、未知数据不静默删除等止损。

### B. 需求真实，但当前实现必须重新审计

- 多线路并发端口：需求成立，但主 `2080` 无条件抢先路由会破坏原生路由语义；
- 配置 merge 防篡改、端口持久化和生命周期 fencing：部分机制有价值，但复杂度必须与实际风险相称；
- Resolve domain 的旧实现确实不安全，暂时禁用是止损；但“人工多入口管理”现在是明确缺失项，不能把禁用入口当作需求完成；
- sing-box fork 是必要实现材料，但其中为 RouteFluent 加入的 fallback/本地解析能力不得暴露为本项目默认。

### C. 非必要或显著增加系统扰动

- 把 persistent Windows Runtime、独立 service、WFP kill-switch、stable anchor 当成本项目核心需求或发布前提；
- 围绕三项局部能力全面重写 GUI/core RPC、退出、导出、测试和 OS 状态模型；
- 把 MultiMapper 或 RouteFluent 的完整产品模型嵌入 NekoRay；
- 用大量新抽象替代上游已经稳定工作的普通路径，却没有对应用户需求和回归证据。

这些内容不是永远禁止研究，但不能再以现行需求名义继续扩建。若已有代码影响上游行为，应优先简化、隔离或回退。

### D. 明确回归，应恢复或修正

- 删除/禁用 external-core、Naive、非 Xray 分享与插件兼容、GeoSite 自动完成、在线更新、手工系统代理等上游能力；URL Test 与 TCP Ping 当前均应保留，但二者的路径语义必须明确区分；
- 仅因名字含 `v2ray` 而删除格式或组件；
- 用无条件 `2080 -> main outbound` 规则遮蔽 NekoRay 原生路由；
- 把诊断、测试机网络或 Clash TUN 特例写入产品默认；
- 因不支持某一新组合而删除 profile 或静默改投其它线路。

恢复时也应按上游基线逐项回移，避免再做一次“整体优化”。

### E. 仍然缺失，后续开发的真实重点

- 可用的多入口查看、刷新、手工固定、解除和分层诊断 UI；
- 多入口选择的持久化与原始域名/SNI 保真；
- 原生 `2080` 与专用线路 listener 的正确分层；
- AnyTLS 全生命周期和 front proxy/detour 的真实闭环；
- 上游误删能力的选择性恢复；
- 以 NekoRay 4.0.1 为对照的配置、数据迁移和 Windows 真实回归。

## 10. 数据与变更安全

- 订阅刷新先完整下载、解析、暂存和校验，再一次性提交；失败时旧 group 不变。
- 未知、旧版或损坏配置保留并提示，禁止静默删除或复用其 ID。
- 保存入口选择、端口映射或 profile 时，失败必须保持磁盘与内存的旧状态可解释。
- 不为了局部需求建立覆盖全部配置的分布式事务平台；先使用上游保存模型和窄范围原子替换。
- 密码、订阅正文、分享链接和完整运行配置不得进入普通日志或 Git。

## 11. 验收矩阵

### 11.1 上游回归

- 使用真实旧配置启动，不丢 profile/group/order/chain/front proxy；
- 上游仍支持的协议、导入导出、分享、路由、测速、外置 core 和手工系统模式不因本分支退化；
- 明确证明删除内容确属 Xray runtime，而非名称相似的兼容功能。

### 11.2 Resolver 与多入口

- 覆盖专用字段 present/empty/invalid/absent 四类；
- 覆盖普通 nameserver 有/无 HTTPS DoH；
- 断开 provider DoH 时观测不到本机/公共 DNS 泄漏；
- 同域名多个 A/AAAA、多个 provider endpoint 和不同 resolver 来源可区分；
- 刷新/诊断不改变 active entry；
- 固定入口后连接 IP 改变而 TLS SNI 保持原域名；
- 任一保存/刷新失败时旧选择和旧订阅不变。

### 11.3 AnyTLS

- native/Mihomo/继承字段 round-trip；
- YAML、分享链接和手工创建结果一致；
- 单独运行、并发专用端口、front proxy/detour 分别验证；
- 非法 client 或不支持组合在启动前精确失败。

### 11.4 多线路并发

- 至少两条不同线路同时通过不同专用端口工作；
- 每个端口只到自己的完整 chain；
- 一条线路、resolver 或 listener 故障不使其它端口重绑；
- 端口冲突不 try-next；
- 专用线路失败时无 direct、主线、其它线路或本机 DNS fallback；
- 原生 `2080` 的上游路由/reject/DNS 行为与 4.0.1 对照一致。

必须区分“源码存在、构建成功、core schema 通过、真实出站闭环、Windows 集成通过”。前两项不能代替真实网络证据。

## 12. 后续开发纪律

1. 先建立 4.0.1 对照和当前差异清单，再改代码。
2. 每个改动绑定本文件的具体条目；无法绑定的默认为范围外。
3. 优先恢复无关回归，再收敛三项核心能力。
4. 修改稳定上游路径时，必须有最小复现和前后回归。
5. 诊断代码与运行选择代码分离，禁止“测试后顺手应用最佳结果”。
6. 新增自动策略、OS 全局副作用、持久 service 或大规模架构替换前，必须由用户另行确认。
7. RouteFluent/MultiMapper 中的 YAML、AnyTLS、Trojan 等材料可以复制为**测试 fixture**，但先去除真实凭据，并记录来源与预期语义；不能直接导入生产秘密。
8. 当实现与本文件冲突时，先停下来修正文档/实现的一致性，不以新增兼容层掩盖冲突。

## 13. 尚未冻结的实现细节

以下问题不能由 agent 自行升级为产品政策：

- 专用端口的默认范围、自动建议方式和 UI 布局；
- 同一 provider 集合中多个 DoH endpoint 的具体可视化；
- 命名入口集合未来是否支持某种显式多 IP 运行策略；
- 哪些外置 core 组合能在第一阶段进入并发托管；
- Windows 最低版本、ARM64、安装器和公开发行方式。

未确认时采用最小、可逆、无自动切换的实现。
