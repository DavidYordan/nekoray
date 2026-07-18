# 外部核心收敛审计

日期：2026-07-18

## 背景

本项目当前目标是以本地化 RouteFluent patched sing-box 作为唯一主线核心，AnyTLS、Clash 订阅、DoH server resolver、主/辅助线路都应围绕 sing-box 生成最终 core 配置。xray/v2ray 已不再适合作为主线核心，Naive 这类只能依赖外部可执行文件的协议也会在 UI 和导入入口制造歧义。

## 本轮结论

1. Hysteria2/TUIC 已有 sing-box 原生 outbound 生成逻辑，应继续保留协议支持。
2. Hysteria2/TUIC 的 `forceExternal` 外部核心路径与当前主线冲突，应删除新入口并忽略历史字段。
3. Naive 当前没有 sing-box outbound 支撑，只能外部 `naive` core 启动；它不应再出现在新建配置和订阅导入中。
4. `extraCore` 设置页不应默认塞入 `naive`、`hysteria2`、`tuic`。只有用户显式配置过非空路径的扩展项才应显示。
5. `Custom (sing-box outbound)` 与 `Custom (sing-box config)` 仍有价值，用于直接填写 sing-box JSON，不属于外部核心冗余。

## 已整改

1. 新建配置下拉框移除了 `naive` 和 `Custom (Extra Core)`。
2. Raw 链接和 Clash 订阅导入会跳过 Naive。
3. Hysteria2/TUIC 的 `NeedExternal()` 固定返回 sing-box 内建路径，旧 `forceExternal` 不再影响运行。
4. Hysteria2/TUIC 编辑页删除 `Force use external core`，`QUICBean` 不再序列化该字段，历史 JSON 中的同名字段会被忽略。
5. `extraCore` 不再自动注入旧预设；空的旧预设会在打开设置页后被清理。
6. 历史 Naive 配置构建时返回明确错误：`Naive is not supported by the RouteFluent sing-box core.`
7. core outbound 构建错误优先级已调整，bean 返回的明确错误会优先于泛化的 `unsupported outbound`。

## 保留兼容

1. `NaiveBean` 类型定义暂时保留，用于历史数据库反序列化和链接导出兼容；正式入口不再生成。
2. `BuildExternal()` 基础设施暂未全量删除，因为历史 `CustomBean` 外部配置仍可能存在。后续若确认完全不需要第三方外部核心，可继续删除 `ExternalBuildResult`、extraCore 设置页和外部进程启动链路。
3. Hysteria2/TUIC 的外部构建函数暂时保留为死路径，避免一次性删除带来过大编译面；运行时不会进入。

## 下一步建议

1. 继续审计 `ExternalBuildResult` 在 `ConfigBuilder` 和 core 启动链路中的使用，确认是否只剩历史 `custom` 兼容。
2. 若用户确认完全禁止任意外部核心扩展，再删除 `extraCore` 设置页、`CustomBean::BuildExternal()`、外部进程启动和对应翻译项。
3. 对已有配置执行一次启动前审计，列出仍含 `type=naive` 或 `custom` 外部核心的历史线路，提示用户迁移或删除。
