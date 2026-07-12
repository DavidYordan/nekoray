# Nekoray 自构建 sing-box 核心适配设计

日期：2026-07-12  
目标读者：Nekoray 后续二次开发者  
状态：调查与设计文档，供后续实现使用。本文不要求保留当前中途改造代码的兼容分支。

## 结论摘要

Nekoray 当前已经朝 sing-box-only 方向改造：README 写明 backend 为 sing-box，`go/cmd/nekobox_core` 也已经是嵌入 sing-box 的 gRPC/CLI wrapper；当前工作树还有一批未提交 AnyTLS 与 sing-box CLI 适配改动。但它的思路仍停留在“官方或 Matsuri sing-box + 旧 Nekoray/V2Ray 语义”阶段，尚未跟上 RouteFluent 自构建 sing-box 的两个核心能力：

1. AnyTLS outbound 必须能显式输出 `client` 字段，例如 `client="mihomo/1.19.28"`，但不能自动猜测或把订阅兼容写成隐式 fallback。
2. 代理 outbound 的 `server` 域名解析必须支持 provider-scoped DoH resolver group：优先 DoH，DoH 全部不可用时才允许可选 local fallback，DoH 恢复后自动优先回到 DoH。

Nekoray 的后续方向应是：**以 RouteFluent 自构建 sing-box 为唯一受控核心，重做 AnyTLS client、代理 server DoH 管理、Clash 订阅预览和分流导入，不再保留 Xray/V2Ray 核心路径或 geodata 主路径污染。**

## 调查依据

本次调查基于 `D:\complex\nekoray` 当前工作树，开始时状态为 `main...origin/main` 且已有未提交改动。未提交改动涉及：

- AnyTLS 新类型与 UI：`fmt/AnyTLSBean.hpp`、`ui/edit/edit_anytls.*`、`fmt/Bean2CoreObj_box.cpp`、`fmt/Link2Bean.cpp`、`fmt/Bean2Link.cpp`、`sub/GroupUpdater.cpp`
- sing-box wrapper：`go/cmd/nekobox_core/core_box.go`、`main.go`、`grpc_box.go`、`singbox_compat.go`
- 构建与打包：`libs/get_source_env.sh`、`libs/build_go.sh`、`build_windows_package.ps1`
- DNS/route 生成：`db/ConfigBuilder.cpp`、`res/vpn/sing-box-vpn.json`

同时参考 RouteFluent 当前权威实现与文档：

- `D:\complex\RouteFluent\docs\ANYTLS_CLIENT_COMPATIBILITY.md`
- `D:\complex\RouteFluent\docs\SING_BOX_SERVER_RESOLUTION_POLICY.md`
- `D:\complex\RouteFluent\third_party\sing-box\README.md`
- `D:\complex\RouteFluent\third_party\sing-box\docs\doh-priority-local-fallback-resolution.md`

RouteFluent 自构建核心仓库与版本：

- 仓库：`https://github.com/DavidYordan/routefluent-sing-box.git`
- tag：`v1.13.12-routefluent-anytls-client.7`
- version：`1.13.12-routefluent-anytls-client.7`
- patch id：`routefluent-anytls-client-dns-resolver-group-check-v1`
- release binary SHA256：`8d142f917518cd1660d7370e61a93defc3b6827681b092c0088a7c4cd4b46331`
- manifest SHA256：`211c5ad3fce08ab72573f6bb5339858cde892f8ab324a7272fe521b8e0585118`
- features：`anytls_outbound_client_field`、`routefluent_dns_resolver_group`、`routefluent_dns_check_start_validation`

## 当前问题

### 1. Core 来源没有收敛到受控自构建 sing-box

当前 `go/cmd/nekobox_core/go.mod` 仍通过：

```go
replace github.com/sagernet/sing-box => ../../../../sing-box
```

引用仓库外部的本地 `sing-box` 目录；`libs/get_source.sh` 仍从 `https://github.com/MatsuriDayo/sing-box.git` clone 并 checkout `libs/get_source_env.sh` 中的 commit。这样构建出的 `nekobox_core` 是否具备 AnyTLS `client` 和 `routefluent_resolver_group`，完全取决于开发机旁边的源码状态，缺少可审计、可复现、可发布的核心来源。

后续必须固定为 RouteFluent 自构建 sing-box。不能再使用官方 sing-box、Matsuri sing-box、Xray 或其它 fork 作为主路径核心。

### 2. AnyTLS 只完成了基础协议，缺少 client 控制

当前 `AnyTLSBean` 已包含 password、TLS、Reality、session 参数，`BuildCoreObjSingBox()` 能生成：

```json
{
  "type": "anytls",
  "server": "...",
  "server_port": 443,
  "password": "...",
  "tls": {}
}
```

但缺少：

- `anytls_client_mode`
- `anytls_client_value`
- `client` 编译输出
- UI 控件
- Clash/link 导入证据
- package smoke 中对 `client` 字段的 `nekobox_core check -c`

这会导致 NEX 类线路仍无法表达 `client="mihomo/1.19.28"`，也无法让用户在 native/mihomo/custom 之间做显式选择。

### 3. Clash 订阅解析只导入 proxies，丢弃 DNS 与分流图

`sub/GroupUpdater.cpp::RawUpdater::updateClash()` 当前只读取 YAML 顶层 `proxies`，并按节点类型生成 Nekoray profile。它忽略：

- `dns`
- `proxy-groups`
- `rules`
- `rule-providers`
- `proxy-providers`
- `nameserver-policy`
- Clash provider 私有 rule-set

这意味着含 DoH 的 Clash 订阅被导入后，代理 server 仍按 Nekoray 全局 DNS 或本机 DNS 解析；含分流策略的 Clash 订阅也会被静默降级为“节点列表”。这不是可靠兼容。

后续必须提供导入预览和证据面板，不能继续静默丢弃。

### 4. DNS 生成仍是全局 remote/direct DNS，不是线路级 server resolver

`db/ConfigBuilder.cpp` 当前生成：

- `dns-remote`
- `dns-direct`
- `dns-local`
- route `default_domain_resolver={"server":"dns-direct"}`

这些是 Nekoray 的全局业务 DNS/route DNS 思路，不等于“解析代理 outbound server 域名”的 provider-scoped DoH 逻辑。

RouteFluent 当前模型要求：

- outbound `server` 是 IP：不生成 resolver。
- outbound `server` 是域名且没有供应商 DoH：生成显式 `local_only` resolver group。
- outbound `server` 是域名且用户选择供应商 DoH：生成 `routefluent_resolver_group`，primary 为供应商 DoH，local fallback 是否允许由用户显式决定。
- 每个域名 outbound 必须写入 `domain_resolver.server`，不能依赖全局 `route.default_domain_resolver` 偷跑。

### 5. Xray/V2Ray 残留会污染后续判断

当前没有发现实际 Xray core 主进程路径，但仍有大量 Xray/V2Ray 历史残留：

- README Credits 仍列 Xray/V2Ray core。
- 构建脚本仍下载 `geoip.dat`、`geosite.dat`。
- UI route autocomplete 仍使用 Qv2ray `GeositeReader` 读取 v2ray dat。
- 内部类名仍有 `V2rayStreamSettings`，注释和配置字段仍按 v2rayN/V2Ray 术语描述。
- `README.md` 仍把订阅格式描述为 v2rayN 与 Clash 混合格式。

如果目标是 Nekoray 的新主线，Xray/V2Ray 不应再作为核心或配置模型出现。历史 link 格式导入可以作为边界输入存在，但必须在进入内部模型时转换为 sing-box 当前字段。

## 目标原则

1. 唯一核心：Nekoray 主路径只使用 RouteFluent 自构建 sing-box。官方 sing-box、Matsuri sing-box、Xray、V2Ray 不能作为 AnyTLS/DoH 主路径 fallback。
2. 显式能力：AnyTLS `client` 和 resolver group 必须由 `nekobox_core check -c` 或构建 manifest 证明，不能只看版本号。
3. 明确模型：内部 profile 字段使用当前模型名，不再用 `client` 旧字段、Xray 字段或 Clash 原始字段直接污染持久层。
4. 不自动猜测：订阅导入不能自动把 NEX/AnyTLS 设置为 mihomo client；不能自动把 Clash 顶层 DNS 写入所有节点。
5. DoH 是代理入口解析策略，不是业务 DNS 分流策略。Clash `dns.nameserver` 可以成为候选，但必须人工确认后才绑定到域名节点的 `server_resolver`。
6. local fallback 只限同一 resolver group 的代理 server 域名解析，不是 outbound 自动换线、route fallback 或 direct 放行。
7. 分流必须分级导入：能准确承载才导入；不能准确承载就作为 unsupported evidence 展示，不能静默降级。

## Core 接入设计

### 推荐方案：source-based 嵌入构建

Nekoray 当前 `nekobox_core` 是 Go 程序，直接 import sing-box library。它不是简单启动外部 `sing-box` 二进制。因此推荐继续嵌入，但把 sing-box 源码来源改成 RouteFluent 自构建源码：

1. 增加子模块：

```bash
git submodule add https://github.com/DavidYordan/routefluent-sing-box.git third_party/routefluent-sing-box
git -C third_party/routefluent-sing-box checkout v1.13.12-routefluent-anytls-client.7
```

2. 构建前运行 `third_party/routefluent-sing-box/build_routefluent_sing_box.py` 的 source preparation 阶段，生成已打补丁的 sing-box 工作树。
3. 将 `go/cmd/nekobox_core/go.mod` 的 replace 指向该已打补丁工作树，例如：

```go
replace github.com/sagernet/sing-box => ../../../third_party/routefluent-sing-box/work/<patched-sing-box>
```

具体相对路径由后续开发者按最终目录确定。关键要求是：`go build nekobox_core` 必须链接到 RouteFluent patched sing-box，而不是旁边随意 clone 的 `../../../../sing-box`。

如果 `routefluent-sing-box` 后续提供完整 patched source branch，也可以直接 replace 到该 branch/submodule；这比临时引用 build wrapper 的 work 目录更干净。

### 可选方案：external binary 模式

也可以把 Nekoray 改成启动 RouteFluent release 的 `sing-box` 二进制，而不是嵌入 sing-box library。但这会影响现有 gRPC stats、`boxapi`、latency test、proxy HTTP client 和 core lifecycle。除非后续愿意重做 core 控制层，否则不建议优先采用。

### 必须补齐的构建门禁

`build_windows_package.ps1` 和 `libs/build_go.sh` 必须新增以下 smoke：

1. `nekobox_core version` 输出必须包含 `1.13.12-routefluent-anytls-client.7` 或等效 feature manifest。
2. `nekobox_core check -c anytls-client-check.json` 必须接受：

```json
{
  "type": "anytls",
  "tag": "anytls-out",
  "server": "example.com",
  "server_port": 443,
  "password": "secret",
  "client": "mihomo/1.19.28",
  "tls": { "enabled": true, "server_name": "example.com" }
}
```

3. `nekobox_core check -c routefluent-dns-resolver-group-check.json` 必须接受 valid resolver group。
4. 以下 invalid config 必须失败：
   - local resolver 被放进 regular primary。
   - HTTPS resolver 被放进 fallback。
   - `fallback_enabled=true` 但缺少 `probe_domains`。
5. Windows/Linux package 都必须携带构建 manifest 或在 About/Core 页面展示 features。

当前 RouteFluent release 主要发布 Linux amd64 二进制；Nekoray Windows 版若要使用同一 patch，必须从 patched source 构建 `nekobox_core.exe`，或扩展 RouteFluent core workflow 发布 Windows/amd64 资产。

## 数据模型设计

### AnyTLS client 字段

在 `fmt/AnyTLSBean.hpp` 增加：

```cpp
QString anytlsClientMode = "native"; // native | mihomo | custom
QString anytlsClientValue = "";
```

持久化字段建议：

```cpp
_add(new configItem("anytls_client_mode", &anytlsClientMode, itemType::string));
_add(new configItem("anytls_client_value", &anytlsClientValue, itemType::string));
```

严格规则：

- `native`：不输出 `client`。
- `mihomo`：输出 `client="mihomo/1.19.28"`。
- `custom`：`anytlsClientValue` 必须是非空 visible ASCII，建议长度 1..128，输出到 `client`。
- 不接受旧 `client` 字段作为持久字段。
- 从 Clash 或 share link 读到 `client` 时，应作为候选证据展示；只有用户确认或显式导入策略允许时才写入当前字段。

`BuildCoreObjSingBox()` 伪代码：

```cpp
if (anytlsClientMode == "native") {
    // omit client
} else if (anytlsClientMode == "mihomo") {
    outbound["client"] = "mihomo/1.19.28";
} else if (anytlsClientMode == "custom" && validVisibleAscii(anytlsClientValue)) {
    outbound["client"] = anytlsClientValue;
} else {
    result.error = "invalid AnyTLS client mode/value";
}
```

### 代理 server resolver 字段

该能力不限于 AnyTLS，理论上应适用于所有 sing-box outbound whose `server` can be a domain：AnyTLS、Trojan、VLESS、VMess、Shadowsocks、SOCKS、HTTP 等。建议在 `AbstractBean` 或可复用子结构中加入：

```cpp
QString serverResolverDohUpstreams = ""; // newline-separated HTTPS DoH endpoints
bool serverResolverAllowLocalFallback = true;
```

语义：

- `serverAddress` 是 literal IP：不使用 resolver 字段；UI 应提示或禁用。
- `serverAddress` 是域名且 `serverResolverDohUpstreams` 为空：生成 `local_only` group。
- `serverAddress` 是域名且 `serverResolverDohUpstreams` 非空：生成 provider DoH group。
- `serverResolverAllowLocalFallback=false`：DoH 不可用时解析失败关闭。

不要把该字段放进 `custom_config`，否则 UI、订阅导入、导出和测试都无法统一校验。

## sing-box config 生成设计

### 目标输出

当一个 outbound 的 `server` 是域名，并绑定供应商 DoH：

```json
{
  "type": "anytls",
  "tag": "proxy",
  "server": "edge.provider.example",
  "server_port": 443,
  "password": "secret",
  "client": "mihomo/1.19.28",
  "domain_resolver": {
    "server": "rf-resolver-provider-a",
    "strategy": "ipv4_only"
  },
  "tls": {
    "enabled": true,
    "server_name": "edge.provider.example"
  }
}
```

对应 DNS section：

```json
{
  "dns": {
    "servers": [
      { "tag": "local-system", "type": "local" },
      {
        "tag": "provider-a-doh-1",
        "type": "https",
        "server": "1.1.1.1",
        "server_port": 443,
        "path": "/dns-query"
      },
      {
        "tag": "rf-resolver-provider-a",
        "type": "routefluent_resolver_group",
        "primary": ["provider-a-doh-1"],
        "fallback": "local-system",
        "fallback_enabled": true,
        "probe_domains": ["www.gstatic.com", "cloudflare.com"],
        "failure_threshold": 2,
        "recovery_success_threshold": 2,
        "probe_interval": "30s",
        "unhealthy_cooldown": "20s",
        "fallback_ttl_cap": "60s"
      }
    ]
  }
}
```

无供应商 DoH 的域名 outbound：

```json
{
  "tag": "rf-resolver-no-doh-local",
  "type": "routefluent_resolver_group",
  "mode": "local_only",
  "primary": [],
  "fallback": "local-system",
  "fallback_enabled": true,
  "fallback_ttl_cap": "60s"
}
```

### 生成流程

建议改造 `db/ConfigBuilder.cpp`：

1. `BuildChainInternal()` 生成 outbound 时，不要立即只依赖全局 DNS；同时收集 `ResolverBindingRequest`：

```cpp
struct ResolverBindingRequest {
    QString outboundTag;
    QString server;
    QStringList dohUpstreams;
    bool allowLocalFallback;
    QString sourceKey; // subscription/group/provider evidence
};
```

2. 所有 outbounds 生成完后，统一构建 resolver groups：
   - always add `local-system`。
   - DoH endpoint 去重，生成稳定 tag。
   - 按 `sourceKey + dohUpstreams + allowLocalFallback` 分组生成 `routefluent_resolver_group`。
   - 无 DoH 的域名节点共享 `rf-resolver-no-doh-local`。
3. 回写每个 outbound 的 `domain_resolver.server`。
4. DoH endpoint 的 host 是域名时，给该 DoH server 设置 bootstrap resolver，推荐 `domain_resolver="local-system"` 并设置 TLS `server_name`。
5. route 层若仍需要 `default_domain_resolver`，应明确使用与业务 route DNS 对应的 resolver；不能拿 provider resolver group 当全局 route resolver。
6. 每次运行前必须调用 `nekobox_core check -c` 或内部等效 check；失败直接阻断启动。

### DoH URL 校验

合法 DoH URL：

- scheme 必须是 `https`。
- 必须有 host。
- path 必须非空且不能只是 `/`。
- 不允许 username/password。
- 不允许 query。
- 不允许 fragment。

示例：

- 合法：`https://1.1.1.1/dns-query`
- 合法：`https://dns.google/dns-query`
- 非法：`https://dns.google/`
- 非法：`http://1.1.1.1/dns-query`
- 非法：`https://user:pass@dns.example/dns-query`

## Clash 订阅导入设计

### 导入模式

必须把导入拆成两个模式：

1. 节点导入模式：导入 proxies 为 Nekoray profiles；DNS、proxy-groups、rules 作为 evidence 展示，只有用户确认的 DoH/client 候选会写入节点。
2. 完整 Clash 策略导入模式：尝试把 `proxies + proxy-groups + rules + rule-providers` 转为一个 sing-box custom config/profile。不能准确转换时阻断或标记 unsupported，不能静默降级。

当前 Nekoray 的 profile 模型更接近节点导入模式；完整策略导入需要单独设计 UI 与存储。

### DNS 候选提取

从 Clash YAML 提取 DoH 候选时建议分级：

| Clash 字段 | 用途判断 | 默认动作 |
| --- | --- | --- |
| `dns.proxy-server-nameserver` | 最接近“代理 server 域名解析器” | 高置信候选，仍需人工应用 |
| `dns.nameserver` | 通用 DNS，可能被用于代理解析 | 中置信候选，仅展示 |
| `dns.default-nameserver` | DoH bootstrap 或传统 DNS | 不作为 provider DoH 自动应用 |
| `dns.fallback` | Clash 流量 DNS fallback | 不作为 proxy server resolver |
| `dns.direct-nameserver` | direct 流量 DNS | 不作为 proxy server resolver |
| `dns.nameserver-policy` | 域名级 DNS 策略 | 作为 routing/DNS evidence，不自动应用 |

只接受合法 HTTPS DoH URL。`udp://`、`tls://`、纯 IP DNS、`dhcp://` 可以展示为 evidence，但不能进入 RouteFluent resolver group 的 `primary`。

### 候选应用规则

导入预览必须展示：

- 候选 DoH URL。
- 来源字段，例如 `dns.proxy-server-nameserver[0]`。
- 命中的域名节点数量。
- literal IP 节点数量。
- 已选择应用范围。

应用时：

- 只能写入 `serverAddress` 为域名的节点。
- literal IP 节点必须跳过。
- 默认不自动启用 local fallback，或由用户在同一操作中显式选择。
- 一套订阅里可能包含多个商家的节点，不能强制一个 DoH 套全局；UI 必须允许按节点、多选、按 server suffix 或按订阅来源分批应用。
- 对已存在手工 resolver 的节点，覆盖前必须二次确认。

### AnyTLS client 候选

Clash AnyTLS proxy 若出现 `client` 或等价字段：

- 不直接写入旧 `client` 字段。
- 作为 `anytls_client_mode/custom` 候选展示。
- 如果值等于 `mihomo/1.19.28`，可提示可映射为 `mihomo` mode。
- 用户确认后才写入 `anytls_client_mode` 与 `anytls_client_value`。

`client-fingerprint` 是 uTLS fingerprint，不是 AnyTLS `client` 广播值，不能混淆。

### Clash 分流承载判断

当前 Nekoray `Routing` 模型有：

- direct/proxy/block domain/ip 文本列表。
- custom route JSON。
- global route JSON。
- sing-box geosite/geoip db。

它不能天然完整承载 Clash 策略图，尤其是 `proxy-groups`、`rule-providers` 和远程 provider 更新。

建议预览时将 Clash 分流分成四类：

1. 可直接转换的 public rules：
   - `DOMAIN`
   - `DOMAIN-SUFFIX`
   - `DOMAIN-KEYWORD`
   - `IP-CIDR`
   - `IP-CIDR6`
   - `GEOIP`
   - `GEOSITE`，前提是目标 sing-box geosite 数据源存在。
2. 可作为 sing-box route custom JSON 的 rules：
   - `PROCESS-NAME`
   - `PROCESS-PATH`
   - `DST-PORT`
   - `NETWORK`
   - 需要逐项用目标 `nekobox_core check -c` 验证。
3. 需要 rule_set 的 private rules：
   - `RULE-SET`
   - `rule-providers`
   - remote/local provider
   - 只有在目标 sing-box v1.13.12 patched core 确认支持对应 `rule_set` 类型和格式时才能导入。
4. 不支持或需人工重建的 rules：
   - `SCRIPT`
   - `AND/OR/NOT` 复杂组合，若不能准确翻译。
   - provider 依赖外部更新、认证、非标准 format。

对 proxy group 的处理：

- `select` 可映射为 sing-box `selector` outbound，但 UI 必须提供选择器状态。
- `url-test` 可映射为 sing-box `urltest` outbound，但这属于自动选线，节点导入模式不能默认启用。
- `fallback`、`load-balance` 只有在语义确认等价并且用户选择“完整策略导入”时才允许。
- `DIRECT`、`REJECT`、`PASS` 等动作必须精确映射；不能把未知 group 默认指向当前选中 profile。

导入结果必须显示“保真度”：

- `exact`：全部规则和 group 可等价生成。
- `partial`：只导入节点，规则未启用。
- `blocked`：存在会改变流量语义且无法承载的规则，必须阻断完整策略导入。

## UI 设计

### AnyTLS 编辑器

在 `EditAnyTLS` 中新增一个“Client identity”区域：

- Segmented control：
  - Native
  - Mihomo compatible
  - Custom
- Custom 输入框：
  - 只在 Custom 模式启用。
  - 校验 visible ASCII 和长度。
- 只读 evidence：
  - 当前 sing-box core 是否支持 `anytls_outbound_client_field`。
  - 当前配置最终会输出的 `client` 值；Native 显示为 omitted。

保存前必须校验；校验失败不关闭对话框。

### Server Resolver 通用区域

建议在所有有 `serverAddress/serverPort` 的 profile 编辑器中加入通用折叠区，而不是只放 AnyTLS：

- Mode：
  - Local system DNS for proxy server
  - Provider DoH for proxy server
- DoH upstream 列表：
  - 添加/删除/粘贴多行。
  - 每行即时校验。
- Allow local fallback：
  - 仅 Provider DoH mode 下显示。
  - 关闭表示 DoH 全部不可用时失败关闭。
- Advanced：
  - probe domains，默认 `www.gstatic.com`、`cloudflare.com`。
  - strategy，默认 `ipv4_only`。

当 server 是 literal IP：

- 显示“IP server does not need resolver”。
- 禁用 DoH 设置。
- 保存时不输出 resolver group。

### Clash 导入预览

订阅导入不要直接入库，应先打开预览窗口：

Tabs：

1. Nodes：节点列表，显示协议、server 是否域名、AnyTLS client 候选、resolver 状态。
2. Provider DoH：从 Clash DNS 提取的 DoH 候选，显示来源和可应用节点数量。
3. Routing：proxy-groups、rules、rule-providers 的解析结果与可承载性。
4. Unsupported：所有丢弃项和原因。

关键动作：

- Apply DoH to selected domain nodes。
- Apply DoH to all domain nodes in this subscription。
- Apply AnyTLS client candidate to selected AnyTLS nodes。
- Import nodes only。
- Import full strategy as custom sing-box config。

不能存在“无提示直接导入并丢弃 DNS/rules”的路径。

### Core/Diagnostics 页面

新增核心能力显示：

- core version。
- patch id。
- `anytls_outbound_client_field`。
- `routefluent_dns_resolver_group`。
- `routefluent_dns_check_start_validation`。
- 最近一次 `check -c` 结果。

运行中的配置详情应能展示 resolver group 摘要：

- group tag。
- mode。
- primary DoH endpoints。
- local fallback。
- probe domains。
- 绑定 outbound。

如果 sing-box 后续提供逐次查询 telemetry，再接入健康状态和 fallback/recovery 事件；在此之前不要从日志猜测单次解析路径作为权威状态。

## Xray/V2Ray 清理设计

必须删除或隔离：

1. README 中 Xray/V2Ray core credits 与“backend 可切换”暗示。
2. 构建脚本中的 Xray/V2Ray core 下载、打包、fallback 路径。
3. `geoip.dat`、`geosite.dat` 作为主路径资源。主路径只用 sing-box `geoip.db`、`geosite.db` 或 sing-box rule_set。
4. UI 中直接读取 v2ray dat 的 route autocomplete。可替换为 sing-box geosite db reader，或暂时移除 autocomplete。
5. 内部命名中的 `V2rayStreamSettings` 应重命名为 `TransportSettings` 或 `SingBoxTransportSettings`。
6. 注释中的“v2ray outbound tag”“v2ray Subdomain”等应改为当前 sing-box 术语。

可以保留但必须隔离：

- v2rayN share link 导入/导出：仅作为边界格式兼容，进入内部后必须转换成 sing-box 模型。
- Qv2ray 的 JSON editor 组件：如果只是 Qt UI widget，可暂时保留，但应移出命名空间暴露或替换，避免开发者误以为核心仍与 Qv2ray/V2Ray 绑定。
- Shadowsocks `v2ray-plugin`：这是插件名称，不等于 V2Ray core。若保留，必须在文档中说明它只是 Shadowsocks plugin 字段。

不应保留：

- Xray/V2Ray core 下载、运行、配置生成、fallback。
- 任何“sing-box 不支持则换 Xray 跑”的路径。
- 任何把 Xray/V2Ray 配置 JSON 直接并入 sing-box 主路径的兼容层。

## 实现阶段建议

### 阶段 0：冻结和备份当前中途改造

当前工作树已有较大未提交改动。后续开发者接手前应先：

1. 建立分支或 commit 当前状态。
2. 标记哪些改动可保留。
3. 不要在脏改动上继续混写 UI、core 和订阅解析。

### 阶段 1：核心来源收敛

1. 引入 `routefluent-sing-box` 子模块或固定 source provider。
2. 修改 Go replace，确保 `nekobox_core` 编译到 patched sing-box。
3. 移除 `libs/get_source.sh` 对 Matsuri sing-box 的主路径依赖。
4. package smoke 增加 AnyTLS client 与 resolver group valid/invalid checks。
5. About/Core 页面显示 feature evidence。

验收：stock sing-box 或未打补丁 sing-box 无法通过 package smoke。

### 阶段 2：AnyTLS client 模型

1. 扩展 `AnyTLSBean`。
2. 扩展 AnyTLS UI。
3. 扩展 share link parse/export。
4. 扩展 Clash AnyTLS proxy parser，提取 client candidate。
5. `BuildCoreObjSingBox()` 输出 `client`。
6. 增加单元/fixture 测试。

验收：native 省略 `client`；mihomo 输出固定值；custom 输出用户值；非法值失败。

### 阶段 3：server resolver 模型与 config generator

1. 在通用 bean 层加入 server resolver 字段。
2. 改造 ConfigBuilder，按 outbound 域名 server 生成 resolver group。
3. 生成 `domain_resolver.server`。
4. 支持 DoH endpoint bootstrap。
5. 增加 `nekobox_core check -c` fixtures。

验收：域名 server + DoH 生成 provider resolver group；域名 server + 无 DoH 生成 local_only；literal IP 不生成 resolver。

### 阶段 4：Clash 导入预览

1. 重写 `RawUpdater::updateClash()` 为 parse -> preview -> apply。
2. 提取 DoH candidates。
3. 提取 AnyTLS client candidates。
4. 解析 proxy-groups/rules/rule-providers 并输出承载性报告。
5. 默认只导入节点；完整策略导入必须显式选择。

验收：含 `dns.proxy-server-nameserver` 的订阅不会自动写库；用户应用后才写入选中域名节点。

### 阶段 5：Xray/V2Ray 清理

1. 删除 Xray/V2Ray core 文档、构建、包资源。
2. 移除 v2ray dat 主路径。
3. 重命名内部 transport settings。
4. 更新 README 与技术文档。

验收：全仓搜索 `xray` 不再出现主路径代码；`v2ray` 只允许出现在 link-format compatibility 或 Shadowsocks plugin 文案中。

## 测试矩阵

### Core checks

- `nekobox_core version` 显示 RouteFluent patched version/features。
- `nekobox_core check -c anytls-native.json` 通过。
- `nekobox_core check -c anytls-client-mihomo.json` 通过。
- `nekobox_core check -c resolver-provider-doh.json` 通过。
- `nekobox_core check -c resolver-local-only.json` 通过。
- invalid primary local 失败。
- invalid fallback https 失败。
- fallback enabled without probe domains 失败。

### C++ config generation

- AnyTLS native/mihomo/custom 输出正确。
- domain server with DoH 输出 `domain_resolver`。
- IP server 不输出 resolver group。
- 多供应商 DoH 不合并。
- 同 DoH 同 fallback 配置可去重。
- DoH host 为域名时有 bootstrap resolver。

### Clash import

- 仅 proxies 的 Clash：正常导入节点。
- 含 AnyTLS `client`：进入候选，不自动写入。
- 含 `dns.proxy-server-nameserver`：进入高置信 DoH 候选。
- literal IP 节点应用 DoH 时被跳过。
- mixed vendor subscription 可分批应用 DoH。
- 含 `proxy-groups/rules`：预览显示 exact/partial/blocked。
- `RULE-SET` provider 不支持时阻断完整策略导入。

### UI

- AnyTLS client mode 切换后保存/重开一致。
- Custom 非法值不能保存。
- Server resolver DoH URL 非法不能保存。
- Clash preview 导入前不会修改 profile。
- Apply DoH 后只改选中域名节点。

## 最小开发切入点

如果后续开发者只能先做一个最小闭环，建议顺序如下：

1. 先固定 core 到 RouteFluent patched sing-box，并让 package smoke 证明 `client` 和 resolver group。
2. 再做 AnyTLS client UI 与输出。
3. 再做手工 profile 的 server resolver UI 与输出。
4. 最后做 Clash 导入预览和完整分流承载。

不要先重构完整 Clash 分流，也不要先清理所有命名。核心能力和 config check 没有锁住之前，UI 越丰富越容易把错误语义固化。

## 交付边界

本文交付的是设计，不是代码实现。后续实现时必须注意：

- 不要回退当前未提交改动，先单独保存。
- 不要用官方 sing-box 临时跑通 AnyTLS。
- 不要把 Clash DNS 自动应用到所有节点。
- 不要把 Xray/V2Ray 作为运行 fallback。
- 不要用 `custom_config` 偷塞 `client` 或 resolver group 绕过模型层。

完成后，Nekoray 应成为一个基于 RouteFluent 自构建 sing-box 的受控客户端：用户能明确修改 AnyTLS client，能明确管理代理 server 的供应商 DoH，能看到 Clash 订阅中哪些 DNS/路由语义被承载、哪些需要人工确认或不支持。
