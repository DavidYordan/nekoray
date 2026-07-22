# ADR 0003：订阅导入与 server-domain DoH

状态：Accepted（DNS 语义已实现；订阅成功提交事务仍未完成）
日期：2026-07-22

## 决策

- 下载、格式识别、解析、字段校验和差异生成全部成功后才能修改数据库。
- 网络失败、HTML、空响应、坏 YAML/Base64、零支持节点不得改变旧 group、order或profile。
- 候选先在内存 stage；提交串行化并采用原子文件写入。失败保留稳定 ID、统计、本地覆盖、front proxy、chain和运行引用。
- 不支持协议/字段必须统计和展示，禁止静默降级到其它协议或 direct。

## Clash DoH 精确语义

- `dns.proxy-server-nameserver`/`dns.proxy_server_nameserver` 显式存在时是权威来源。只提取合法 HTTPS DoH；只有 UDP/本地项时不借 `dns.nameserver`，节点沿用 NekoRay 原生解析。
- 专用字段完全缺失时，才从 `dns.nameserver` 提取合法 HTTPS DoH。该行为用于兼容确实把线路 server resolver 放在普通 nameserver 中的订阅（当前 NEX）。这不是在专用字段无效时自动 fallback。
- 所选来源中的非法 HTTPS 项使整次导入失败；非 HTTPS 项被忽略并计数。来源、策略版本和最终 DoH 列表按 group 保存，刷新成功才替换旧值。
- 有效 HTTPS DoH只绑定需要解析 server domain的节点；server已是IP或最终没有 provider DoH时沿用 NekoRay普通路径。
- 不解析节点私有的 `server-resolver`/`server_resolver` 扩展，也不接受其中的 local fallback。
- 对由 provider DoH 管理的 server-domain resolver binding 全面关闭 local fallback，辅助绑定尤其不得借主线或本机解析；没有该 provider 字段的普通节点继续沿用 NekoRay 解析路径。
- DoH endpoint 为域名时使用生成配置中的原生 `dns-local` bootstrap，并保留 TLS SNI；不强制地址族。它只解析 DoH endpoint，不允许在 provider DoH 失败后用本机 DNS 解析线路 server。
- custom merge 后必须逐项验证原生 bootstrap、DoH server、strict resolver group 和 outbound binding 未被替换。旧策略保存的非空订阅 DoH 在成功刷新迁移前拒绝使用，避免旧的普通 nameserver 猜测复活。

## AnyTLS client 来源

Clash格式本身不等于某个永久 client身份。导入必须区分节点显式 client、组默认兼容和继承；刷新/分享链接往返不能把显式 native误变成继承，也不能把非法值静默变成另一模式。

`proxy-groups`、rules、providers等不属于当前三项扩展；不因订阅中存在它们就复刻完整 Clash runtime，但必须报告被忽略范围。
