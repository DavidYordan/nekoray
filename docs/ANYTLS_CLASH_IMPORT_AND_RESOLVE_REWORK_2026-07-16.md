# Clash 订阅、AnyTLS Client 与域名解析整改

日期：2026-07-16

## 目标

1. 只要导入来源是 Clash 订阅，无论其中线路是不是 `type: anytls`，该订阅默认 client 身份都应是 Mihomo。
2. 对 AnyTLS 线路，订阅默认 Mihomo 应实际落到 sing-box `client` 行为；对非 AnyTLS 线路，client 字段通常不参与 core 配置，但仍应作为订阅级默认元数据保存，避免同一订阅内部行为割裂。
3. 普通分享链接，包括 `anytls://`，继续保留 sing-box 原生默认行为，除非链接中显式携带 `anytls_client_mode` 或 `anytls_client_value`。
4. Clash 订阅自带、用于解析代理 `server` 域名的 DoH，应优先作为订阅级默认值保存和展示；单线路可覆盖，但默认继承订阅。
5. 右键“解析为 IP”不再直接改写线路；先解析并展示结果，再由用户选择“替换”或“复制新增”。
6. 解析为 IP 必须能显式选择解析来源和出站路径：不通过代理、通过具体运行线路、或通过指定 DoH；不能只隐式使用系统 DNS 或当前主线路。

## 当前状态

- `sub/GroupUpdater.cpp` 负责 Clash YAML 导入。
- `fmt/Link2Bean.cpp` 负责普通分享链接导入；普通 AnyTLS 链接默认 `native`。
- `ui/edit/edit_anytls.ui` 已暴露 `Client Identity` 和 `Custom Client`。
- `ui/edit/dialog_edit_profile.ui` 已暴露单线路 `Server Resolver`、`DoH Upstreams` 和 `Allow Local Fallback`。
- `ui/mainwindow.cpp::on_menu_resolve_domain_triggered` 已改为先选择解析服务和出站路径，再展示结果，并提供 `Replace`、`Copy as new`、`Copy results`。
- 解析服务已支持订阅 DoH、单线路 DoH override、路由 Remote/Direct DoH、公共 DoH、自定义 DoH 和系统 DNS。
- 出站路径已支持不通过本项目代理、通过主线路端口、通过已启动辅助线路端口。
- `fmt/AbstractBean::ResolveDomainToIP` 旧直接替换函数已不在源码中，当前只剩本文档历史记录。

当前缺口：

- 订阅/分组层级已能保存 `source_type`、默认 client、默认 DoH 和 fallback，并在 Clash 导入、分组编辑和 MultiMapper 导出中使用。
- AnyTLS 单线路已能选择 `subscription/native/mihomo/custom`，其中 `subscription` 继承分组默认 client。
- 解析为 IP 已迁移到后台任务，整组/大批量对比解析不会阻塞主界面；任务期间会显示进度，并允许取消。取消会中断等待中的 DoH 请求，系统 DNS 只能在当前系统解析调用返回后停止。
- 尚缺的是更彻底的内部 TUN 重启 fail-closed 方案。

## 订阅应作为一套来源管理

一个订阅链接通常来自同一服务商、同一账号、同一套运行假设。它内部的线路往往应共享：

- client 身份，例如 Mihomo。
- provider DoH 或 server resolver。
- fallback 策略，例如 strict / allow local fallback。
- 订阅流量、过期时间、来源名称。
- 导入类型，例如 Clash、普通 base64、普通链接列表。

如果只在单线路上保存这些字段，会有两个问题：

- 操作成本很高。几十上百条线路逐条改 client 或 DoH 不现实。
- 行为容易不一致。同一订阅里一部分线路继承了 provider DoH，另一部分线路被手工改成系统 DNS，会导致“同一套订阅，某些可用、某些不可用”的疑难问题。

建议新增订阅/分组层级默认值：

```json
{
  "source_type": "clash",
  "defaults": {
    "client": {
      "mode": "mihomo",
      "value": "mihomo/1.19.28"
    },
    "server_resolver": {
      "mode": "doh",
      "doh_upstreams": [
        "https://example.com/dns-query"
      ],
      "fallback": "strict"
    }
  }
}
```

继承模型建议：

- 每条线路默认 `inherit` 订阅 client 和 resolver。
- 单线路允许 `override`，但 UI 必须明确显示“继承订阅”还是“单线路覆盖”。
- 订阅层级修改 client 或 DoH 时，应可选择：
  - 只修改订阅默认值，继承线路立即生效。
  - 覆盖全部线路，把已有单线路 override 一并改掉。
  - 只覆盖当前选中线路。
- 对 Clash 订阅，如果顶层 DNS 和单 proxy `server-resolver` 不一致，默认应以订阅级统一值为主；单 proxy resolver 可作为高级覆盖信息保留并提示用户，而不是默认把一套订阅切碎成很多行为不同的小组。

## Clash 默认 Mihomo 规则

导入来源是 Clash 时：

- 订阅层级默认 client 固定为 `mihomo/1.19.28`。
- AnyTLS 线路如果没有显式 client 字段，继承订阅默认 client。
- AnyTLS 线路如果显式写了 `client`、`anytls-client-mode` 或 `anytls_client_mode`，应按导入策略决定是尊重线路显式值还是统一覆盖。建议默认尊重显式值但在 UI 中提示“与订阅默认不一致”。
- 非 AnyTLS 线路不应强行向 core 写入无意义的 `client` 字段，但应带有“来自 Clash 订阅，默认 client=Mihomo”的来源元数据，供批量修改、导出 MultiMapper、未来协议转换或诊断使用。

导入来源不是 Clash 时：

- 普通 `anytls://` 链接继续默认 `native`。
- Neko 链接按自身保存的字段恢复。
- 普通 base64 链接列表不应被自动视为 Clash，也不应自动套 Mihomo，除非用户在导入时显式选择“按 Clash/Mihomo 订阅处理”。

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

- 保留协议字段、TLS/传输字段、订阅级 client、代理 server 的 provider DoH 有实际价值，因为它们直接决定单条 outbound 能否连通。
- 丢弃 Clash 策略、规则、分组运行态是合理的。本项目已有自己的分组、路由和测试模型；除非未来新增“完整 Clash 运行时导入模式”，否则强行映射 Clash 分组会有损且容易误导。

## 解析为 IP 增强设计

解析结果不同是正常现象。同一个 `server` 域名，通过 Clash 订阅自带 DoH、公共 DNS、系统 DNS、或通过某条代理线路访问 DoH，返回 IP 都可能不同。UI 必须让用户看到并选择这些差异，而不是隐藏在“Auto”里。

建议把解析选项拆成两个维度。

解析服务：

- `订阅 DoH`：当前线路所属订阅的默认 DoH。
- `线路覆盖 DoH`：单线路 override 的 DoH。
- `路由 Remote DNS DoH`：当前路由配置 Remote DNS 中的 HTTPS DoH。
- `路由 Direct DNS DoH`：当前路由配置 Direct DNS 中的 HTTPS DoH。
- `公共 DoH`：用户选择或内置的公共 DoH，例如 Cloudflare、Google、Ali、DNSPod。内置公共 DoH 只能作为诊断选项，不能替代订阅默认 DoH。
- `系统 DNS`：Qt/OS resolver。
- `自定义 DoH`：用户临时输入。

出站路径：

- `不通过本项目代理`：解析请求不使用本项目 core 的代理端口。注意这不等于绕过系统级 TUN；如果 Windows 已被其它软件 TUN 接管，系统路由仍可能经过那个 TUN。
- `通过主线路`：使用当前主运行线路的本地 mixed/socks/http 端口发起 DoH 请求。
- `通过辅助线路`：未来多线路运行后，使用某个已启动辅助端口发起 DoH 请求。
- `通过临时线路`：高级能力，可为未运行的线路临时启动 resolver-only 测试配置，完成解析后关闭。该能力实现复杂，建议晚于辅助线路模型。

组合规则：

- `系统 DNS` 只能用于“不通过本项目代理”。系统 resolver 没有可靠的“通过某条代理线路解析”语义。
- `通过主线路` 或 `通过辅助线路` 时，建议只支持 DoH。实现上可以用 HTTP CONNECT/SOCKS 访问 DoH endpoint，避免 UDP DNS over proxy 的不确定性。
- DoH endpoint 自身是域名时，需要清晰显示 bootstrap 行为。可选策略包括系统 bootstrap、订阅 bootstrap、或 SOCKS 远端解析。诊断结果中必须记录。
- 默认建议：Clash 订阅线路使用“订阅 DoH + 不通过本项目代理”。用户可手动切换为“订阅 DoH + 通过指定线路”，用于模拟某条线路视角下访问 provider DoH 的结果。

解析结果展示：

- 展示每条线路的原始域名、解析服务、出站路径、使用的具体 DoH URL 或 DNS 类型。
- 展示是否通过代理、代理线路名称、代理监听端口。
- 展示返回的全部 IP、首选 IP、耗时、错误信息。
- 多个解析方式对比时，应并列表格显示，方便用户判断 provider DoH 与公共 DNS 的差异。
- 默认仍不直接替换线路地址；用户可选择 `Replace`、`Copy as new`、`Copy results`。

多线路后的代理选择：

- 解析对话框的“通过代理”下拉框必须列出所有运行线路：
  - 主线路：标记 `Main` 和主端口。
  - 辅助线路：标记 `Aux:<port>`。
- 如果没有运行线路，`通过代理` 不应默默退回主线路或系统代理；应提示先启动主线路或辅助线路。
- 如果选中的代理线路停止，解析任务应失败并提示，不应自动换其它线路。

## 建议整改清单

已完成：

- [x] 普通 AnyTLS 链接保留 sing-box 原生默认。
- [x] AnyTLS client 模式 UI 已存在。
- [x] 单线路 provider DoH UI 已存在。
- [x] 右键解析域名已改为预览结果，再选择 `Replace` 或 `Copy as new`。
- [x] 解析为 IP 对话框已拆分“解析服务”和“出站路径”，支持不通过代理、通过主线路、通过辅助线路。
- [x] 解析结果已展示 DoH/公共 DNS/系统 DNS 的差异，并记录代理线路和端口。
- [x] `fmt/AbstractBean::ResolveDomainToIP` 遗留直接替换函数已从源码移除。
- [x] 大批量“解析为 IP”已迁移到后台任务，避免 UI 阻塞。
- [x] 后台“解析为 IP”已增加进度展示和取消能力。
- [x] Clash 订阅导入后，在订阅/分组层级保存 `source_type=clash`。
- [x] Clash 订阅无论协议类型，订阅默认 client 都设为 `mihomo/1.19.28`。
- [x] AnyTLS 线路默认继承订阅 client；非 AnyTLS 线路保留来源默认元数据但不向 core 写无效字段。
- [x] 订阅层级 client/DoH/fallback 管理 UI 已在分组编辑面板提供，并支持重置线路为继承默认值。
- [x] 单线路编辑 UI 已增加“继承订阅 / 覆盖订阅”状态。
- [x] 辅助线路端口已接入右键解析出站选择，并在配置生成时作为独立出站加入统计列表。
- [x] 内部 TUN 正运行时，辅助端口启动/停止会被阻断，避免为了添加/删除辅助线路而隐式重载 sing-box 造成 TUN 短暂卸载。
- [x] 程序自重启已携带本次正在运行的主线路 ID；即使未开启“记住上次代理”，也能在恢复系统代理/TUN 意图时同步恢复主线路。
- [x] 系统代理自动重启路径已改为 fail-closed：程序重启、更新器重启、管理员提权重启不清空 Windows 系统代理。
- [x] 外部 TUN 自动重启/退出路径已改为 fail-closed：不主动停止外部 TUN，只有用户明确关闭 TUN 时才停止。
- [x] 内部 TUN 使用实际运行态标记，普通线路切换、重启代理、路由重载、辅助端口变更、程序重启/更新会被阻断，要求先明确关闭 TUN。
- [x] Windows 无偷跑采样脚本已新增：`tools/verify_fail_closed_restart.ps1`，文档见 `docs/Windows_Fail_Closed_Verification.md`。

待整改：

- [ ] 如未来需要内部 TUN 运行期间无中断切换线路/路由，再设计并验证 Windows kill-switch；当前策略是阻断隐式 stop/start。

## 后续注意

- `anytls://` 标准分享链接能携带 AnyTLS client mode/value，但不会携带本项目的订阅级默认值和 `server_resolver_doh` 等 provider DoH 元数据。
- `nekoray://` 链接能保存 bean JSON，信息更完整，但 MultiMapper 当前不直接消费 `nekoray://`。
- 与 MultiMapper 配合的主格式已改为精简 Clash-compatible YAML，并通过 `x-nekoray` 扩展显式携带订阅级 `source_type`、client 默认值、DoH 默认值、线路字段和必要 override；不复制原始 Clash 全量 YAML。
