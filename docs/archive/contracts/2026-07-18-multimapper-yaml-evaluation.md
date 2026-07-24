# MultiMapper 精简 YAML 对接契约与实现说明

> 状态：历史评估，不是已接受的输出契约。
> 归档日期：2026-07-20
> 替代文档：[产品边界](../../PRODUCT.md)、[最小分支 ADR](../../architecture/decisions/0002-minimal-fork-boundary.md) 与 [范围偏离审计](../audits/2026-07-20-scope-deviation-audit.md)。
> 已知错误：正文把 MultiMapper 导出当作应继续演进的产品功能；该结论已撤销。

日期：2026-07-18

本文记录 Nekoray 到 MultiMapper 的剪贴板导出契约。2026-07-18 用户已确认：主格式从独立 JSON 包切换为“精简 Clash-compatible YAML + `x-nekoray` 扩展元数据”。当前代码路径为 `sub/MultiMapperExport.cpp::BuildMultiMapperExport()`；启用 `yaml-cpp` 时输出 YAML，禁用 YAML 构建时保留旧 JSON fallback。

## 结论

我倾向同意改为精简 YAML，但要明确一点：YAML 是序列化形式，仍然需要一个稳定的最小 schema。建议不要另起完全独立的 `nekoray-multimapper-export` JSON 根结构，而是采用“Clash-compatible YAML + `x-nekoray` 扩展元数据”的方式。

这样做的核心收益是：

- MultiMapper 已经用 `yaml.safe_load()` 识别 `proxies:` 并导入 Clash YAML。
- Nekoray 当前构建已启用 `yaml-cpp`，Clash 导入路径也依赖它。
- 用户、Nekoray、MultiMapper 三方都更容易肉眼检查 YAML。
- `proxies:` 能直接承载线路连接字段，避免把同一条线路重新包装成一套全新的 JSON item schema。

## 为什么不能只导出纯 Clash YAML

纯 Clash YAML 可以承载大多数单线路连接字段，但不能完整表达本项目已经明确需要的订阅级语义：

- `source_type=clash`：MultiMapper 需要知道来源是 Clash，从而默认 AnyTLS client 为 Mihomo。
- 分组/订阅级默认 client：如果只写在每条 AnyTLS 线路上，会增加冗余；如果不写，MultiMapper 无法知道继承语义。
- 分组/订阅级 server resolver：Clash 顶层 `dns.proxy-server-nameserver` 只能表达单个订阅的默认 DoH；多组选中导出时，不同分组的 DoH 无法只靠一个顶层 `dns` 表达。
- `inherit.client` / `inherit.server_resolver`：这不是 Clash runtime 语义，但对 Nekoray 和 MultiMapper 的一致性很重要。
- 订阅流量、过期信息和来源标签：Clash 原生字段没有统一位置。

因此建议是“Clash-compatible”，不是“完整原始 Clash 订阅”，也不是“完全纯 Clash”。

## 推荐 YAML 形态

建议根结构保持 Clash 可识别入口：

```yaml
proxies:
  - name: Hong Kong 01
    type: anytls
    server: example.com
    port: 443
    password: secret
    client: mihomo/1.19.28
    sni: example.com
    client-fingerprint: chrome
    x-source-tag: WD
    x-inherit:
      client: true
      server-resolver: true

dns:
  proxy-server-nameserver:
    - https://example.com/dns-query

x-nekoray:
  format: compact-clash-yaml
  version: 1
  exported_at: "2026-07-18T00:00:00Z"
  groups:
    WD:
      source_type: clash
      source: WD
      defaults:
        client:
          mode: mihomo
          value: mihomo/1.19.28
        server_resolver:
          mode: doh
          doh_nameservers:
            - https://example.com/dns-query
          fallback: strict
```

规则建议：

- `proxies` 使用 Clash 风格字段，尽量让 MultiMapper 现有 `parse_clash_yaml()` 能先读到线路。
- 单分组导出时，可以同步写 `dns.proxy-server-nameserver`，兼容 MultiMapper 当前 DoH 识别逻辑。
- 多分组导出时，每条 proxy 必须带 `x-source-tag`，每个分组的默认 DoH 放入 `x-nekoray.groups.<tag>.defaults.server_resolver`。
- AnyTLS 为兼容现有 MultiMapper，可在 proxy 上内联 `client: mihomo/1.19.28`；同时保留 `x-inherit.client=true` 表达它来自分组默认值。
- `x-` 前缀字段用于扩展元数据。Clash/Mihomo 通常会忽略未知字段，MultiMapper 可以显式读取。

## 与当前 JSON 包对比

当前 JSON 包优点：

- Qt 侧实现简单，现有 `QJsonObject/QJsonDocument` 已完成。
- 类型更确定，字段名不受 YAML 缩进和隐式类型影响。
- 多分组、继承、override 的结构表达很清晰。

当前 JSON 包问题：

- 对 MultiMapper 来说是一个新格式，需要新增一条识别分支。
- 用户无法把它当作 Clash 订阅直观看待。
- 容易形成 Nekoray 和 MultiMapper 之间的专有契约，后续维护者需要同时理解 JSON 包和 Clash YAML 两套结构。

精简 YAML 优点：

- 复用 MultiMapper 已有 Clash YAML 导入入口。
- 对人工排障更友好。
- `proxies` 字段可以接近真实订阅，避免重复设计线路字段。
- 可同时作为剪贴板内容和本地 `.yaml/.yml` 文件导入内容。

精简 YAML 风险：

- 仍需定义 `x-nekoray` 扩展，否则无法表达多分组默认值和继承语义。
- C++ 导出必须使用 `yaml-cpp` emitter 或等价结构化生成，不能手写拼接，避免引号、冒号、换行、非 ASCII 名称破坏 YAML。
- 如果 MultiMapper 只按当前 `dns.proxy-server-nameserver` 读取 DoH，多分组不同 DoH 会丢失；需要 MultiMapper 配合读取 `x-nekoray.groups`。
- 如果为了兼容旧 MultiMapper 在每条 AnyTLS 上重复写 `client`，会牺牲一部分“精简”；如果不重复写，则 MultiMapper 必须先支持分组默认 client。

## 当前实现策略

1. `Copy to MultiMapper` 已切换为输出精简 YAML。
2. 旧 `nekoray-multimapper-export` JSON 函数继续保留，仅作为禁用 `yaml-cpp` 构建时的 fallback 和历史兼容契约。
3. MultiMapper 侧需要优先支持：
   - `x-nekoray.format == compact-clash-yaml`
   - proxy 级 `x-source-tag`
   - `x-nekoray.groups.<tag>.defaults.client`
   - `x-nekoray.groups.<tag>.defaults.server_resolver`
4. 过渡期在 AnyTLS proxy 上内联 `client`，让未完全升级的 MultiMapper 至少不丢 AnyTLS client。
5. 不导出 Clash `proxy-groups`、`rules`、dashboard、health-check、url-test、load-balance/select 等运行时块。

## 已确认的问题

1. 精简 YAML 是 Nekoray 与 MultiMapper 的主格式。
2. 接受 `x-nekoray` 扩展字段，不追求完全纯 Clash YAML。
3. AnyTLS `client` 在过渡期内联到每条 proxy，同时在分组默认值中保留继承来源。
4. 多分组导出时每条 proxy 写 `x-source-tag`，MultiMapper 应按它归组。

最终结论：采用“Clash-compatible YAML + `x-nekoray` 扩展”，不承诺纯 Clash YAML 能表达全部语义。
