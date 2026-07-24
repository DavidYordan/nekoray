# MultiMapper 剪贴板 JSON 导出历史契约

> 状态：已被替代的历史契约。
> 归档日期：2026-07-20
> 替代文档：[产品边界](../../PRODUCT.md)、[最小分支 ADR](../../architecture/decisions/0002-minimal-fork-boundary.md) 与 [范围偏离审计](../audits/2026-07-20-scope-deviation-audit.md)。
> 已知错误：正文把 MultiMapper 描述为现行产品能力；该范围已撤销，正文仅用于历史追溯。

日期：2026-07-17

本文记录 Nekoray 早期实现过的 MultiMapper 精简 JSON 导出格式。2026-07-18 的实现记录将主格式切换为“精简 Clash-compatible YAML + `x-nekoray` 扩展”，详见 [历史 YAML 评估](./2026-07-18-multimapper-yaml-evaluation.md)。当前 `Copy to MultiMapper` 默认输出 YAML；本文仅用于历史兼容和无 `yaml-cpp` 构建 fallback。

## 入口

当前 Nekoray 提供两个入口：

1. 线路列表右键 `Share -> Copy selected to MultiMapper`
   导出当前选中的多条线路。
2. 未选中线路时打开 `Share -> Copy current group to MultiMapper`，或在分组编辑面板点击 `Copy to MultiMapper`
   导出当前分组/订阅的全部线路。

现有普通分享链接和 Neko Links 入口保留，不受影响。

## 根结构

```json
{
  "format": "nekoray-multimapper-export",
  "version": 1,
  "exported_at": "2026-07-17T00:00:00Z",
  "groups": {
    "WD": {
      "source_type": "clash",
      "source": "WD",
      "defaults": {
        "client": {
          "mode": "mihomo",
          "value": "mihomo/1.19.28"
        },
        "server_resolver": {
          "mode": "doh",
          "doh_nameservers": [
            "https://example.com/dns-query"
          ],
          "fallback": "strict"
        }
      },
      "doh_nameservers": [
        "https://example.com/dns-query"
      ],
      "items": []
    }
  }
}
```

`groups` 的 key 应被 MultiMapper 视为 `source_tag`。Nekoray 默认使用分组名作为 key，并在重名时追加后缀。

## 分组字段

- `source_type`：`clash`、`subscription` 或 `manual`。Clash 订阅必须保留为 `clash`。
- `source`：默认写分组名，不默认写原始订阅 URL，避免剪贴板泄漏。
- `defaults.client`：订阅级 client。Clash 订阅默认 `mihomo/1.19.28`，即使导出的线路不是 AnyTLS 也保留该元数据。
- `defaults.server_resolver`：订阅级 server resolver。DoH 来自 Clash 顶层 `dns.proxy-server-nameserver` 或回退的 HTTPS nameserver。
- `doh_nameservers`：兼容 MultiMapper 当前 `source_tag` 元数据的 DoH 列表，应与 `defaults.server_resolver.doh_nameservers` 保持一致。
- `subscription_info`：可选，保存流量和过期元数据。
- `items`：线路列表。

## 线路字段

每个 item 至少包含：

```json
{
  "protocol": "anytls",
  "address": "server.example.com",
  "port": 443,
  "tag": "Hong Kong 01",
  "settings": {},
  "streamSettings": {},
  "anytlsSettings": {},
  "inherit": {
    "client": true,
    "server_resolver": true
  },
  "nekoray": {
    "profile_id": 12
  }
}
```

规则：

- `settings` 和 `streamSettings` 尽量贴近 MultiMapper 当前线路字典结构。
- AnyTLS 专有 idle-session 字段放在 `anytlsSettings`。
- 不再导出 `share_link`，避免重复携带完整分享链接造成体积膨胀。
- `inherit.client=true` 表示该线路继承分组默认 client。
- `inherit.server_resolver=true` 表示该线路继承分组默认 DoH。
- `client_override` 只在 AnyTLS 单线路显式覆盖订阅 client 时出现。
- `server_resolver_override` 只在单线路显式覆盖订阅 DoH 时出现。

## 不导出的内容

以下内容不进入精简包：

- 原始 Clash `proxy-groups`
- Clash `rules` / `rule-providers`
- Clash dashboard、external-controller、secret
- Clash health-check、url-test、load-balance/select 运行时语义
- Nekoray 本机系统代理、TUN、测速结果、UI 排序等本地状态
- 原始订阅 URL，除非后续增加明确的高级选项

## MultiMapper 接收建议

MultiMapper 可以按以下方式接入：

1. 识别 `format == "nekoray-multimapper-export"`。
2. 遍历 `groups`，把 group key 作为 `source_tag`。
3. 读取 `doh_nameservers` 并写入当前 `Globals.doh_nameservers[source_tag]`。
4. 读取 `subscription_info` 并写入当前 `Globals.subscription_info[source_tag]`。
5. 遍历 `items`，为每条线路补上 `source_tag` 和 `source` 后复用现有 `add_row()` 或 `add_nodes_from_subscription()`。
6. 对 AnyTLS，若线路没有 `client_override`，使用 `defaults.client.value`；若有 override，则以 override 为准。
7. 对域名 server，优先按 `source_tag` 使用 `doh_nameservers` 生成 RouteFluent runtime resolver group。

当前 MultiMapper 生产配置生成器主要支持 `anytls`、`trojan`、`shadowsocks`、`socks/socks5`。其它协议可以先进入表格，但生成生产 sing-box 配置前应明确提示不支持，而不是静默降级为 direct。
