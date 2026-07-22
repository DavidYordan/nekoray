# NekoRay Windows 私人分支

这是基于 NekoRay 4.0.1 的 Windows 私人二次开发项目。目前处于接管整改期，**不可发布，也不能替换本机生产 NekoRay**。

## 产品边界

本分支遵循“最小化扩展”原则：NekoRay 原有能力默认保留；只有 Xray 核心因不支持 AnyTLS 被明确删除。核心新增需求有三项：

1. AnyTLS 协议支持；
2. 多线路并发：主 Mixed 端口和各辅助 Mixed 端口分别绑定确定的逻辑线路；
3. 按字段存在性读取 Clash 自带的 server-domain DoH：专用 `dns.proxy-server-nameserver` 存在时以它为准；它完全缺失时才使用 `dns.nameserver` 中的 HTTPS DoH。

2026-07-22 另明确追加一个私人导出便利功能：线路右键多选在保留上游含 remark 链接和 Neko Links 的同时，新增不含 URI fragment 的原生链接，以及严格的 `ip:port:user:pass` 凭据列表；完整边界见[产品契约](docs/PRODUCT.md#34-批量分享格式2026-07-22-用户明确追加)。

旧能力只有在证明与上述扩展存在真实冲突后，才可以提出窄范围修改；不能因名称含 `v2ray`、依赖外置 core，或为了本机测试方便而删除、改写或硬编码产品行为。

## 冻结约束

- 仅支持 Windows，项目纯私人使用。
- 默认主 Mixed 端口为 `12080`。
- `D:\Program Files\nekoray`、端口 `2080` 和它的生产 TUN 属于另一实例，永远不得由本项目停止、修改或接管。
- `auto_detect_interface` 只承担 NekoRay 原有的产品 TUN 防环路语义，不负责按 Mixed 端口选线路。测试环境绕行只能存在于显式、临时的测试副本中。
- 只有用户的精准手动操作可以启停系统代理或 TUN。退出、重启、切线和配置重载不得改变这些 OS 模式。
- 线路失败必须失败关闭，尤其辅助线路不得回落到主线路、`direct`、本机 DNS 或其它可用线路。

## 当前判断

- 标准生成配置中的 `12080 -> 主线路` 和 `辅助端口 -> 对应辅助线路` 已成立；空链会拒绝构建。顶层 `custom_config` 合并前会快照每个受管 Mixed 的完整 listener 和从该入口可达的全部 outbound 对象，合并后要求对象逐项一致并保留精确、无条件的入口绑定；这不是对任意自定义路由/DNS 的全局安全证明。完整负向回归与运行时事务切换仍是阻断项。
- 2026-07-20 的历史隔离探针表明临时 Mixed 能处理 HTTP、CONNECT 和 SOCKS5h；在该次 `52080 -> 指定 outbound` 实验中，AnyTLS(Mihomo client) 与 Trojan 单独可用、组合 detour 失败。旧探针对临时副本强制了 `auto_detect_interface=true`，因此这只是组合归因，不能证明产品 `12080`/辅助端口映射或当前导出策略；仍须按 preserve 默认值重跑并做 Windows 集成验收。
- 上一阶段误删了 external-core、Naive、部分分享链接/插件兼容、GeoSite 自动完成和 URL Test 等 NekoRay 能力；URL Test 已按有界测试配置恢复，其余仍须选择性恢复。
- 接管工作树已把单文件保存改为带 durable before/after intent 的原子替换，停止删除未知 profile，并生成可验证 backup/quarantine；订阅刷新改为解析、暂存和校验成功后再提交，部分跨文件创建/删除/移动也已接入事务。剩余保存路径、非空 group、图形恢复和完整模型级事务仍未完成。
- 越界的复杂批量 resolver/change-IP 平台已移除；旧 **Resolve domain** 因使用 Windows 系统 DNS 并永久改写节点域名而暂时禁用，避免破坏 provider DoH。
- 配置导出默认是“不启动线路、剥离产品 TUN/辅助运行态并拒绝已知 OS 副作用”的审计导出；测试模式只允许有界生成配置。TCP Ping 因使用系统直连 socket 已禁用，改用 URL Test。
- TUN 复选框现区分用户期望状态与当前 worker 回报，但后者不是 Windows OS 事实。core 崩溃后只重启空控制 core，不自动恢复 profile/TUN；TUN 下切线/退出仍靠禁止操作规避，没有独立 Windows fail-closed 层，因此不满足需求。
- Windows GUI 现忽略 CRT `SIGTERM`/`SIGINT`，避免该窄入口绕过 UI 退出 guard 并直接带走内部 TUN；强制结束、崩溃、系统关机以及 GUI 与 worker 同生共死的问题仍未解决，不能把这一止损当作持久保护。
- `nekobox_core run/check` 是供构建、审计和隔离测试显式调用的高级入口，不是普通 GUI 操作路径。GUI 使用 localhost、每会话随机令牌和每 daemon UUID 控制 core；所有 RPC 在 handler 前验证精确实例，`GetDaemonInfo` 身份/协议握手通过后才 ready，Start/Stop 响应不确定时使用同一 executor 内的高序号屏障对账。协议 v3 可取消未准入命令并仲裁 Start 取消/发布，但这些仍只是进程内 fence；Go core 尚未重复执行 C++ 产品策略校验，也没有持久 Runtime/WFP/OS 事实源。

完整证据见 [接管状态](docs/TAKEOVER_STATUS.md) 和 [偏离审计](docs/archive/audits/2026-07-20-scope-deviation-audit.md)。

## 构建

```powershell
.\build_windows_package.ps1
```

打包脚本发现目标部署目录仍有运行实例时会直接失败，不会关闭或强杀 GUI/core。详细依赖和限制见 [Windows 构建](docs/development/BUILD_WINDOWS.md)。

当前版本已完成一次不带 Skip 参数的本地完整打包：GUI 同轮生成于 `build-package-windows64/` 并复制到 `deployment/windows64/`，core 直接输出到 `deployment/windows64/`，随后二者进入 zip。deployment/zip 仍是被忽略的本地审计产物，不是 release manifest 或可发布包，也不得据此替换生产安装。

## 文档入口

- [产品契约](docs/PRODUCT.md)
- [接管状态](docs/TAKEOVER_STATUS.md)
- [已知问题](docs/KNOWN_ISSUES.md)
- [推进路线](docs/ROADMAP.md)
- [尚待确认的少量决策](docs/DECISIONS_NEEDED.md)
- [文档索引](docs/README.md)

## 上游与主要组件

本项目源自 NekoRay/NekoBox，并使用 Qt、gRPC、sing-box、anytls/sing-anytls、yaml-cpp 等组件。产品契约只允许 RouteFluent patched sing-box 承载 AnyTLS client 与 server-domain DoH；当前 fork 仍有已隔离但尚未删除的 fallback 实验残余，发布前必须清除。使用该 fork 不意味着推翻 NekoRay 的外置核心或协议模型。
