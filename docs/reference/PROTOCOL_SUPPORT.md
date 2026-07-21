# 协议与能力证据矩阵

状态：接管基线；“上游应保留”“当前有代码”“已经验收”是三件不同的事。
最后更新：2026-07-20

| 能力 | NekoRay基线 | 当前分支实现 | 实测证据 | 接管处理 |
|---|---|---|---|---|
| AnyTLS | 无 | Bean/UI/链接/Clash/core均有 | Mihomo client单跳三协议204；经Trojan detour EOF | 三项扩展，修复组合与继承 |
| Trojan | 有 | 有 | OpenWrt单跳三协议204 | 保留 |
| Shadowsocks | 有 | 有，但v2ray-plugin导入/UI回归 | 未复验 | 恢复兼容并测试 |
| SOCKS/HTTP | 有 | 有，部分userinfo兼容回归 | Mixed入口协议fixture有基线 | 恢复兼容并测试 |
| VMess/VLESS | 有 | 有，v2rayN分享格式回归 | 未复验 | 保留；不等于恢复Xray core |
| Hysteria2/TUIC | 有 | sing-box路径有，外核选项被删 | 未复验 | 恢复上游外核能力 |
| Naive | 有（外置 core） | 模型/UI/执行被误删 | loader 已保留旧文件并防止 ID 复用；schema/UI/执行未恢复 | P0恢复 |
| custom external core | 有 | 被误删 | 未复验 | P0恢复 |
| internal custom/full | 有 | 普通 custom 受最终 validator 约束；`internal-full` 在产品 TUN、辅助映射运行及 latency/full-test 中拒绝，安全文件导出仍可用 | 导出 OS 副作用 fixture 有窄覆盖，运行未完整复验 | 保留并隔离，不允许绕过受管并发契约 |
| URL Test | 有 | 已恢复；使用显式有界生成配置，产品 TUN requested 或 worker 活动时拒绝，测试运行中也不能启 TUN；超时/取消后等真实 worker 退出才释放阻断 | 配置路径有代码审计，真实矩阵未完成 | 保留并补自动回归 |
| Full Test | 有 | 使用同一有界配置；会走系统 DNS 的入口 IP 查询已禁用；空配置/非法 URL、取消与响应体边界已硬化 | Go 窄单测与 race 测试通过，真实线路矩阵未完成 | 保留并补集成回归 |
| TCP Ping | 有 | GUI/core 两层明确禁用；该实现打开系统直连 socket，不能证明所选 outbound | 禁用路径有代码证据 | 不作为线路测试，使用 URL Test |
| GeoSite自动完成 | 有 | reader被删，UI数据为空 | 无 | 对现用 `.db` 重建/替代 |

Xray运行核心保持删除。名称含 `v2ray` 的格式、插件或生态兼容不自动属于Xray核心。

每个协议的正式证据至少覆盖：新建、持久化、编辑、订阅/链接round-trip、core schema、主/辅助端口、HTTP/CONNECT/SOCKS5h、失败关闭、Windows系统代理/TUN。OpenWrt只提供配置/outbound层证据。
