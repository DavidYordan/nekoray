# 订阅导入规则

## 目标安全流水线

```text
下载与HTTP校验
  -> 内容类型/体积/HTML检查
  -> 内存格式识别与解析
  -> 至少一个支持节点 + 字段校验
  -> 差异与丢弃项报告
  -> 串行提交 + 原子保存
```

任一步失败都必须保持旧 group、order、profile文件、稳定ID、本地覆盖和运行引用不变。零支持节点不是“成功的空订阅”。

## Clash server-domain DoH

只读取以下两个等价键：`proxy-server-nameserver` 与 `proxy_server_nameserver`。不得扩展到普通 `dns.nameserver`。

```yaml
dns:
  proxy-server-nameserver:
    - https://resolver.example/dns-query
```

- `dns.nameserver`不作兜底。
- 只接受合规HTTPS DoH；字段存在但没有有效项必须显示错误。
- DoH只应用于需要解析proxy `server`域名的节点。
- 由该 provider DoH 扩展管理的 server-domain outbound 会绑定精确 strict resolver；legacy fallback 字段只为读取旧数据保留且不生效，主线和辅助线的这些受管绑定都禁止 local fallback。没有 provider DoH 的普通节点继续使用 NekoRay/sing-box 原有解析路径。
- DoH endpoint为域名时的bootstrap属于单独控制面策略，当前未最终冻结。
- 过渡实现不会使用本机 DNS bootstrap：URL host 不是 IP 时导入数据可保留，但最终配置构建会明确失败。
- 节点级私有 `server-resolver`/`server_resolver` 不在支持范围内，不能借此打开 local fallback。

## AnyTLS client

节点显式client、组级兼容默认和继承标志必须分别保存。不能简单把“Clash来源”永久等同于Mihomo；不能把非法client静默退成native；链接/刷新round-trip必须保真显式native。

## 不复刻完整 Clash runtime

本扩展不承诺导入 `proxy-groups`、rules、rule-providers、dashboard或health-check运行语义。解析器必须统计并报告忽略项，不能无提示声称完整兼容。

## 当前状态

接管工作树已实现 parse/stage-before-mutate：Clash root/proxies、节点 endpoint 和精确 DoH 字段先验证；解析失败或零支持节点不创建组、不改 order/profile。成功候选才逐个写入并提交元数据。

这还不是完整事务：`AddProfile`、group 保存和旧 profile 删除是多个独立操作，全局 ProfileManager mutation 仍需串行化；崩溃或磁盘故障可能留下新增、重复或残留节点。完成故障注入与单事务提交前，不能把手动或定时刷新标记为正式验收。详见 [ADR 0003](../architecture/decisions/0003-subscription-import-policy.md)。
