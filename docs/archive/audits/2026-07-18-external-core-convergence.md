# 外部核心收敛审计

> 状态：历史审计。旧文对“不支持配置会被忽略”的判断已经失效；当前加载器可能直接删除未知或加载失败的 profile。
> 归档日期：2026-07-20
> 状态：已被用户明确推翻。本文把“删除 Xray”错误扩大为“删除全部外置 core/Naive”，不得继续作为实现依据。
> 替代文档：[ADR 0002](../../architecture/decisions/0002-minimal-fork-boundary.md)、[范围偏离审计](2026-07-20-scope-deviation-audit.md)。
> 注意：正文仅用于说明错误决策如何形成，不代表当前需求或验收结果。

日期：2026-07-18

## 背景

本项目当前目标是以本地化 RouteFluent patched sing-box 作为唯一主线核心，AnyTLS、Clash 订阅、DoH server resolver、主/辅助线路都应围绕 sing-box 生成最终 core 配置。xray/v2ray 已不再适合作为主线核心，Naive 这类只能依赖外部可执行文件的协议也会在 UI 和导入入口制造歧义。

## 本轮结论

1. Hysteria2/TUIC 已有 sing-box 原生 outbound 生成逻辑，应继续保留协议支持。
2. Hysteria2/TUIC 的旧外部核心路径与当前主线冲突，必须删除新入口并忽略历史字段。
3. Naive 当前没有 sing-box outbound 支撑，只能外部 `naive` core 启动；它不应再出现在新建配置和订阅导入中。
4. `extraCore` 与任意第三方外部核心扩展会让正式构建边界变模糊，应从数据模型、设置页、配置构建和启动链路中移除。
5. `Custom (sing-box outbound)` 与 `Custom (sing-box config)` 仍有价值，用于直接填写 sing-box JSON，不属于外部核心冗余。

## 已整改

1. 新建配置下拉框移除了 `naive` 和 `Custom (Extra Core)`。
2. Raw 链接和 Clash 订阅导入会跳过 Naive。
3. Hysteria2/TUIC 固定走 sing-box 内建路径，旧外部核心字段不再序列化。
4. Hysteria2/TUIC 编辑页删除 `Force use external core`，`QUICBean` 不再序列化该字段，历史 JSON 中的同名字段会被忽略。
5. 删除 `extraCore` 数据模型、Basic Settings 的 Extra Core 页、`BuildExternal()` 抽象、协议外部构建函数、`extRs` 结果集合和 gRPC 启动/测速里的外部进程处理。
6. 删除 `NaiveBean`、Naive 分享链接解析/导出和 Naive sing-box 构建函数；Raw/Clash 导入仍显式跳过 `naive+` / `type: naive`。
7. core outbound 构建错误优先级已调整，bean 返回的明确错误会优先于泛化的 `unsupported outbound`。
8. `CustomBean` 只保留 `internal` 与 `internal-full` 两类 sing-box JSON；历史外部 custom core 会在构建/编辑时明确提示不支持。

## 保留兼容

1. 历史配置文件中的 `type=naive` 会按未知/不支持配置处理，不再保留可编辑或可运行的 Naive 协议模型。
2. 历史配置文件中的 `extraCore`、`forceExternal`、`cmd`、`mapping_port`、`socks_port` 等字段会被新版本忽略，不再参与保存和运行。

## 下一步建议

1. 对已有配置执行一次启动前审计，列出仍含 `type=naive` 或非 `internal/internal-full` custom core 的历史线路，提示用户迁移或删除。
2. 后续如需支持新协议，应先确认 RouteFluent/sing-box core 原生支持，再新增 bean 与 sing-box outbound 生成逻辑，不得通过第三方外部可执行文件绕行。
