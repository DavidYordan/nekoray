# 订阅导入规则

状态：现行
最后更新：2026-07-22

## 目标安全流水线

```text
下载与HTTP校验
  -> 内容类型/体积/HTML检查
  -> 内存格式识别与解析
  -> 至少一个支持节点 + 字段校验
  -> 差异与丢弃项报告
  -> UI 线程提交串行化 + 身份/序列化快照重验
  -> 目标：单事务候选提交（尚未完成）
```

任一步失败都必须保持旧 group、order、profile文件、稳定ID、本地覆盖和运行引用不变。零支持节点不是“成功的空订阅”。

## Clash server-domain DoH

解析器按字段是否存在选择唯一来源：

1. `proxy-server-nameserver` 或 `proxy_server_nameserver` 显式存在：它是权威来源，只取其中 HTTPS DoH，不查看普通 `nameserver`。
2. 两个专用字段都完全缺失：从普通 `nameserver` 提取 HTTPS DoH。
3. 选定来源没有 HTTPS DoH：不建立 provider resolver，节点使用 NekoRay/sing-box 原生解析。

```yaml
dns:
  proxy-server-nameserver:
    - https://resolver.example/dns-query
```

- `dns.nameserver` 只在专用字段 absent 时参与，不能在专用字段存在但只含 UDP/本地项时被借用。
- 只接受合规 HTTPS DoH；所选来源中出现语法非法的 HTTPS 项会显示错误并中止刷新。非 HTTPS 项只计数和忽略。
- DoH只应用于需要解析proxy `server`域名的节点。
- 由该 provider DoH 扩展管理的 server-domain outbound 会绑定精确 strict resolver；legacy fallback 字段只为读取旧数据保留且不生效，主线和辅助线的这些受管绑定都禁止 local fallback。没有 provider DoH 的普通节点继续使用 NekoRay/sing-box 原有解析路径。
- DoH endpoint 为域名时，生成器用原生 `dns-local` bootstrap 解析 endpoint host，保留 TLS SNI 且不强制 `ipv4_only`。它不会在 provider DoH 失败时替线路 server 做本机 DNS fallback。
- group 会保存 `origin` 与 resolver policy version。旧版本留下的非空订阅 DoH 在成功刷新前拒绝构建；不得凭旧值猜测它来自哪个字段。
- 顶层 custom config 若替换 `dns-local`、provider DoH、strict group 或 outbound resolver binding，最终构建失败。
- 节点级私有 `server-resolver`/`server_resolver` 不在支持范围内，不能借此打开 local fallback。

## AnyTLS client

节点显式client、组级兼容默认和继承标志必须分别保存。不能简单把“Clash来源”永久等同于Mihomo；不能把非法client静默退成native；链接/刷新round-trip必须保真显式native。

## 不复刻完整 Clash runtime

本扩展不承诺导入 `proxy-groups`、rules、rule-providers、dashboard或health-check运行语义。解析器必须统计并报告忽略项，不能无提示声称完整兼容。

## 当前状态

接管工作树已实现 parse/stage-before-mutate：Clash root/proxies、节点 endpoint 和精确 DoH 字段先验证；解析失败或零支持节点不创建组、不改 order/profile。发起网络请求前会按值捕获不可变 HTTP 选项，包括是否使用代理、代理端口/凭据、User-Agent 与 insecure-TLS；同时记录目标 group 的对象身份、ID、完整序列化字节，以及全部现有成员的 ID、对象身份、group ID、tombstone 和序列化字节。下载/解析可以并行；进入模型提交后调度到 UI 线程并取得参与 mutation 路径的提交串行化 mutex，逐项重验 group 和完整成员集合/顺序/状态均未变化。该 mutex 不是覆盖整个模型的通用读写锁。新建 group 的文件和 `groups/pm.json` 会在同一事务创建。core 运行时若旧节点仍是 auxiliary，删除会失败关闭并保留该节点。

这还不是完整候选事务：刷新内部仍按多个 `AddProfile`、group 保存和旧 profile 删除提交，崩溃或磁盘故障可能留下新增、重复或残留节点；受保护引用或运行中 auxiliary 也尚未在任何写入前整体预检。完成最终集合构造、单事务提交和故障注入前，不能把手动或定时刷新标记为正式验收。详见 [ADR 0003](../architecture/decisions/0003-subscription-import-policy.md)。
