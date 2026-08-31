# 协议与能力证据矩阵

状态：现行证据索引；“上游应保留”“当前有代码”“已经验收”是三件不同的事。
最后更新：2026-08-31

| 能力 | NekoRay基线 | 当前分支实现 | 实测证据 | 接管处理 |
|---|---|---|---|---|
| AnyTLS | 无 | Bean/UI/链接/Clash/core均有 | 提交 `f298d46`：Windows 回环中由 ProfileManager/ConfigBuilder 生成 Mihomo-client AnyTLS，经 Trojan group front proxy 到独立 HTTP origin 成功；停止 Trojan 后 AnyTLS server/origin 仍存活而该端口失败。2026-07-20 真实远端组合 EOF 仍待 preserve 重跑 | 三项扩展；继续验证真实组合与继承 |
| Trojan | 有 | 有 | 提交 `f298d46` 已作为 Windows 回环 AnyTLS 前置代理实际承载流量并完成故障隔离；2026-07-20 OpenWrt 结果仅作历史背景，真实远端组合改在隔离 Windows 重测 | 保留 |
| Shadowsocks | 有 | 有，但v2ray-plugin导入/UI回归 | 未复验 | 恢复兼容并测试 |
| SOCKS/HTTP | 有 | 有；2026-08-31 已恢复 legacy SOCKS base64 `user:password` 解析，HTTP 路径未改 | 严格 base64/UTF-8/分隔符保留与规范链接 round-trip 纯测试、Mixed 入口协议 fixture 有基线；真实 GUI 导入未验收 | 保留并继续补 Windows GUI 证据 |
| VMess/VLESS | 有 | VMess v2rayN base64 JSON 基线导入/导出、持久设置与现代 URI 分流已恢复；VLESS 运行路径未改 | 脱敏字段 round-trip、错误输入、增量 GUI 构建和 CTest 通过；真实 GUI/线路未复验 | 保留；分享格式不等于恢复 Xray core；`vcn/pcs` 扩展仍待单独建模 |
| Hysteria2/TUIC | 有 | sing-box路径有，外核选项被删 | 未复验 | 恢复上游外核能力 |
| Naive | 有（外置 core） | 模型/UI/执行被误删 | loader 已保留旧文件并防止 ID 复用；schema/UI/执行未恢复 | P0恢复 |
| custom external core | 有 | 被误删 | 未复验 | P0恢复 |
| internal custom/full | 有 | 普通 custom 受最终 validator 约束；`internal-full` 在产品 TUN、辅助映射运行及 latency/full-test 中拒绝，安全文件导出仍可用 | 导出 OS 副作用 fixture 有窄覆盖，运行未完整复验 | 保留并隔离，不允许绕过受管并发契约 |
| URL Test | 有 | 已恢复；使用显式有界生成配置。产品 TUN requested/active 时的额外拒绝仍待上游回归审计；超时/取消后等真实 worker 退出才释放活动标记 | 配置路径有代码审计，真实矩阵未完成 | 保留；缩窄无产品依据的 guard |
| Full Test | 有 | 使用同一有界配置；会走系统 DNS 的入口 IP 查询已禁用；空配置/非法 URL、取消与响应体边界已硬化 | Go 窄单测与 race 测试通过，真实线路矩阵未完成 | 保留并补集成回归 |
| TCP Ping | 有 | 2026-08-31 已恢复：通过当前 Windows 网络路径直连 profile server，不创建临时 proxy Box；UI tooltip 明确它不是所选 outbound 的 URL Test | Go 回环 listener 回归与 GUI 增量构建通过；真实 GUI/远端/TUN 未验收 | 保留为只读 server 可达性诊断，不驱动自动选择 |
| GeoSite自动完成 | 有 | reader被删，UI数据为空 | 无 | 对现用 `.db` 重建/替代 |

Xray运行核心保持删除。名称含 `v2ray` 的格式、插件或生态兼容不自动属于Xray核心。

VMess v2rayN 恢复以其官方 base64 JSON schema 和上游 `adef6cd` 为边界。`add` 原样承载 IP 或域名，不解析 DNS；`port`/`aid` 同时读取 JSON 字符串或整数，导出为 v2rayN 要求的字符串；`net=h2` 只在格式边界转换为内部 `http` transport。项目 Bean 已有对应字段的 `alpn`、`fp`、`insecure` 同步保真。v2rayN 当前 schema 另有 `vcn`/`pcs` 证书验证字段，而当前 NekoRay Bean/runtime 没有等价所有权；本切片不猜测映射，也不据此恢复 Xray runtime。官方说明：<https://github.com/2dust/v2rayn/wiki/Description-of-VMess-share-link>。

每个协议的正式证据至少覆盖：新建、持久化、编辑、订阅/链接 round-trip、core schema、主/辅助端口、HTTP/CONNECT/SOCKS5h、失败关闭、Windows 系统代理/TUN。后续只接受 Windows 证据；既有 OpenWrt 记录仅为历史背景，不再重跑。
