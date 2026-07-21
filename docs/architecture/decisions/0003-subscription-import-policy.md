# ADR 0003：订阅导入与 server-domain DoH

状态：Accepted（实现尚未完全满足）
日期：2026-07-20

## 决策

- 下载、格式识别、解析、字段校验和差异生成全部成功后才能修改数据库。
- 网络失败、HTML、空响应、坏 YAML/Base64、零支持节点不得改变旧 group、order或profile。
- 候选先在内存 stage；提交串行化并采用原子文件写入。失败保留稳定 ID、统计、本地覆盖、front proxy、chain和运行引用。
- 不支持协议/字段必须统计和展示，禁止静默降级到其它协议或 direct。

## Clash DoH 精确语义

- 只读取 `dns.proxy-server-nameserver` 或 `dns.proxy_server_nameserver`；不接受其它模糊“等价”字段。
- `dns.nameserver` 服务于普通 DNS，不是 proxy server domain resolver 的兜底。
- 字段 absent、存在且有效、存在但全部非法必须区分；最后一种情况要报错，不能静默本机解析。
- 有效 HTTPS DoH只绑定需要解析 server domain的节点；server已是IP或订阅没有该字段时沿用 NekoRay普通路径。
- 不解析节点私有的 `server-resolver`/`server_resolver` 扩展，也不接受其中的 local fallback。
- 对由 provider DoH 管理的 server-domain resolver binding 全面关闭 local fallback，辅助绑定尤其不得借主线或本机解析；没有该 provider 字段的普通节点继续沿用 NekoRay 解析路径。
- DoH endpoint域名 bootstrap必须采用另行确认、可审计的策略；在此之前，URL host 为域名会在配置构建阶段明确失败。

## AnyTLS client 来源

Clash格式本身不等于某个永久 client身份。导入必须区分节点显式 client、组默认兼容和继承；刷新/分享链接往返不能把显式 native误变成继承，也不能把非法值静默变成另一模式。

`proxy-groups`、rules、providers等不属于当前三项扩展；不因订阅中存在它们就复刻完整 Clash runtime，但必须报告被忽略范围。
