# ADR 0002：最小化 NekoRay 分支与核心边界

状态：Accepted
日期：2026-07-20

## 决策

本项目继续以 NekoRay 4.0.1 的产品思想、数据模型和既有能力为基础，只做三项扩展：AnyTLS、多线路并发、Clash proxy-server-nameserver 对代理 server domain 的 DoH 解析。

- 明确删除 Xray 运行核心，因为它不支持 AnyTLS。
- sing-box 主核心替换为受控的 RouteFluent patched sing-box，以承载上述窄扩展。
- NekoRay 原有 external-core 抽象、Naive、custom external core、TUIC/Hysteria2 可选外核、分享格式、插件兼容、路由工具和测速能力默认保留。
- 外置 core 暂时无法加入单进程并发配置时，可以在托管并发模式中明确拒绝该组合；不能删除协议模型、忽略旧字段或删除 profile。

## 禁止的推导

“Xray 被删除”不等于：

- 删除所有名称含 `v2ray` 的分享格式或 Shadowsocks 插件；
- 删除全部外置 core；
- 删除 Naive；
- 强制所有 NekoRay 功能迁移为 sing-box 原生 outbound；
- 将本分支定义成永久单核心产品。

## 数据兼容

旧 profile、未知类型和暂时不可运行的组合必须可读取、编辑、导出并给出明确状态。加载失败不得自动删除文件。恢复执行能力与实现并发适配可以分阶段进行，但 schema/data preservation 是前置门。

## 实现影响

提交 `d385a17`、`844d9b2`、`4d68e93`、`96f1166` 的 external-core/Naive 收敛方向与本 ADR 冲突，需要选择性恢复；不能盲目整体回滚覆盖 AnyTLS、DoH 和并发改动。详细清单见 [范围偏离审计](../../archive/audits/2026-07-20-scope-deviation-audit.md)。
