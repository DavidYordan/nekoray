# AnyTLS Clash 导入与域名解析整改

日期：2026-07-16

## 目标

1. Clash 订阅导入 `type: anytls` 时，如果订阅没有显式声明 AnyTLS client 字段，默认使用 Mihomo 客户端身份。
2. 普通分享链接，包括 `anytls://`，继续保留 sing-box 原生默认行为，除非链接中显式携带 `anytls_client_mode` 或 `anytls_client_value`。
3. AnyTLS client 身份必须能在单条线路编辑面板里查看和修改。
4. Clash 订阅自带、用于解析代理 `server` 域名的 DoH 必须能在单条线路编辑面板里查看和修改。
5. 右键“解析为 IP”不再直接改写线路；先解析并展示结果，再由用户选择“替换”或“复制新增”。

## 当前状态

- `sub/GroupUpdater.cpp` 负责 Clash YAML 导入。
- `fmt/Link2Bean.cpp` 负责普通分享链接导入；普通 AnyTLS 链接默认 `native`。
- `ui/edit/edit_anytls.ui` 已暴露 `Client Identity` 和 `Custom Client`。
- `ui/edit/dialog_edit_profile.ui` 已暴露 `Server Resolver`、`DoH Upstreams` 和 `Allow Local Fallback`。
- `ui/mainwindow.cpp::on_menu_resolve_domain_triggered` 已改为先选择解析器、再展示结果，并提供 `Replace`、`Copy as new`、`Copy results`。
- `fmt/AbstractBean::ResolveDomainToIP` 这个旧函数仍保留在源码中，内部仍使用 `QHostInfo` 并会直接替换 `serverAddress`。当前主 UI 路径已不再依赖它，后续清理废弃代码时应删除或改造成统一解析服务，避免未来误用。

## Clash 字段支撑

已支持并映射：

- 通用字段：`name`、`server`、`port`。
- 订阅解析器：单代理 `server-resolver` / `server_resolver`；否则使用顶层 Clash `dns.proxy-server-nameserver` 或 `dns.nameserver`。
- Shadowsocks：`cipher`、`password`、`plugin`、`plugin-opts`、`udp-over-tcp`、`smux.enabled`。
- SOCKS/HTTP：`username`、`password`、`tls`、`skip-cert-verify`。
- Trojan/VLESS：`password` 或 `uuid`、`flow`、`network`、`sni` / `servername`、`alpn`、`client-fingerprint`、`packet-addr`、`smux.enabled`、`ws-opts`、`grpc-opts`、`reality-opts`。
- VMess：`uuid`、`alterId`、`cipher`、`network`、`sni` / `servername`、`alpn`、`tls`、`skip-cert-verify`、`client-fingerprint`、`xudp`、`packet-addr`、`smux.enabled`、`ws-opts`、`grpc-opts`、`h2-opts`、`http-opts`。
- Hysteria2：`ports`、`skip-cert-verify`、`ca-str`、`sni`、`obfs-password`、`password`、`up`、`down`。
- TUIC：`uuid`、`password`、`heartbeat-interval`、`udp-relay-mode`、`congestion-controller`、`disable-sni`、`reduce-rtt`、`skip-cert-verify`、`alpn`、`ca-str`、`sni`、`udp-over-stream`、`ip`。
- AnyTLS：`password`、`skip-cert-verify` / `insecure`、`disable-sni` / `disable_sni`、`ca-str` / `certificate`、`sni` / `servername` / `server_name`、`alpn`、`client-fingerprint` / `fingerprint` / `utls_fingerprint`、idle-session 字段、`anytls-client-mode` / `anytls_client_mode`、`anytls-client-value` / `anytls_client_value`、`client`、`reality-opts`。

按设计丢弃：

- Clash `proxy-groups`、`rule-providers`、`rules`、全局代理模式、Clash DNS 分流策略、fake-ip 策略、health-check/url-test 策略、load-balance/select 运行时语义，以及 dashboard/runtime-only 设置。
- 这些字段不是单条出站线路能独立表达的连接字段。把它们塞进本项目的普通订阅导入，会混淆“线路导入”和“完整 Clash 运行时模拟”，也会和 NekoRay 自己的分组、路由、测试模型冲突。

评价：

- 保留协议字段、TLS/传输字段、AnyTLS client 字段、代理 server 的 provider DoH 有实际价值，因为它们直接决定单条 outbound 能否连通。
- 丢弃 Clash 策略、规则、分组运行态是合理的。本项目已有自己的分组、路由和测试模型；除非未来新增“完整 Clash 运行时导入模式”，否则强行映射 Clash 分组会有损且容易误导。

## 域名解析设计

解析器选项：

- `Auto`：优先使用当前线路的 provider DoH；没有 provider DoH 时使用系统解析器。
- `Profile DoH only`：只使用当前线路 `Server Resolver` 中配置的 DoH。
- `Routing Remote DNS DoH`：使用当前路由配置里的 Remote DNS，且仅当它是 HTTPS DoH。
- `Routing Direct DNS DoH`：使用当前路由配置里的 Direct DNS，且仅当它是 HTTPS DoH。
- `System resolver`：使用 Qt/系统解析器。

建议：

- 默认使用 `Auto`。
- 对 Clash 导入且带 provider DoH 的节点，`Auto` 应优先走 provider DoH，使代理 server 域名解析结果符合订阅提供方意图。
- 不应静默替换线路地址。必须先展示解析结果，再让用户选择替换原线路或复制新增线路。

## 已完成清单

- [x] Clash AnyTLS 导入在没有显式 client 字段时默认 `mihomo`。
- [x] 普通 AnyTLS 链接保留 sing-box 原生默认。
- [x] AnyTLS client 模式 UI 已存在。
- [x] provider DoH UI 已存在。
- [x] 标签和 tooltip 已说明 client 与 provider DoH 语义。
- [x] 右键解析域名已改为预览结果，再选择 `Replace` 或 `Copy as new`。
- [x] 已执行针对性构建和测试。

## 后续注意

- `anytls://` 标准分享链接能携带 AnyTLS client mode/value，但不会携带本项目的 `server_resolver_doh` 等线路级 provider DoH 元数据。
- `nekoray://` 链接能保存 bean JSON，信息更完整，但 MultiMapper 当前不直接消费 `nekoray://`。
- 如果要和 MultiMapper 形成可靠配合，应新增“精简线路包”导出格式，显式携带 `source_tag`、线路字段、AnyTLS client、provider DoH，而不是复制原始 Clash YAML。
