# 项目开发原则

日期：2026-07-16

本文记录本项目在 AnyTLS / sing-box 方向整改后的固定开发原则。后续改动、构建和排障都应遵守，避免重复踩坑。

## 核心取舍

1. 本项目以新版 sing-box 核心为唯一主线，AnyTLS 支持必须基于本地化的 RouteFluent patched sing-box。
2. 不再引入、恢复或依赖 xray/v2ray 相关核心路径。无法支撑 AnyTLS 的遗留核心会制造歧义，应继续清理。
3. 上游 sing-box 源码和补丁应本地化管理，构建脚本优先复用 `third_party/routefluent-sing-box` 与项目内缓存，不能每次临时下载。
4. 缺失依赖优先放入项目目录，例如 `qtsdk/`、`libs/deps/`、`tools/`、`third_party/`，避免污染全局环境。

## Windows 构建规则

1. `build_windows_package.ps1` 是 Windows 正式构建入口。不要要求用户手工设置 PATH、手工复制 Qt/MinGW DLL、手工替换核心。
2. 构建脚本必须只处理本项目目录下的部署实例：`deployment/windows64`。不得关闭或修改生产实例，例如 `D:\Program Files\nekoray`。
3. 构建前必须先收敛本项目运行进程，再备份 `deployment/windows64/config`，然后删除并重建部署目录。
4. 构建完成后必须恢复 `deployment/windows64/config`，确保用户测试订阅、分组、线路和本地设置不因打包丢失。
5. 构建失败时也要尽量恢复已备份的配置。不能因为中途失败让部署目录只剩空配置或日志。
6. 正式 zip 默认不携带本机用户配置；本地可测试的 `deployment/windows64` 目录必须恢复用户配置。

## 运行与测试规则

1. 本机可能同时运行旧版生产 Nekoray 的 TUN 模式。除非用户明确要求，不得停止 `D:\Program Files\nekoray` 下的进程。
2. 本项目测试实例只允许操作 `D:\complex\nekoray\deployment\windows64` 下的进程。
3. 重启本项目实例前要考虑系统代理/TUN 的流量偷跑风险。关闭系统代理或关闭 TUN 必须是用户明确动作，不能作为普通重载副作用。
4. 新增后台任务必须保证失败路径清理状态。例如测速、导入、构建、核心启动失败后不能留下永久 busy 标志。
5. 容易误触、会对整个订阅批量发起请求、或依赖不稳定核心状态的 UI 功能，应默认收敛或移除入口。
6. 自重启、管理员提权重启、更新器重启等程序重启链路必须携带本次正在运行的主线路 ID；不能只恢复系统代理/TUN 意图而不恢复实际线路。
7. 在没有 sing-box 热更新或 Windows kill-switch 方案前，辅助端口启动/停止不得在内部 TUN 正运行时隐式重载整个 core；应阻断并提示用户，避免 TUN 被 stop/start 短暂卸载。

## 订阅与配置规则

1. 一个订阅链接应视为一套来源。Clash 订阅的 client、DoH、fallback 等默认值应优先在订阅/分组层级统一管理。
2. Clash 类型订阅无论内部协议是否 AnyTLS，默认 client 身份均为 Mihomo。
3. AnyTLS 线路默认继承订阅 client 和 server resolver；单线路可以覆盖，但 UI 必须明确显示继承还是覆盖。
4. 导入 Clash 订阅时，只保留对本项目有实际运行价值的连接字段、TLS/传输字段、AnyTLS client、provider DoH 等信息。
5. Clash 的分组、规则、rule-provider、health-check、url-test 等运行时语义默认不映射到本项目，避免和本项目自有分组/路由/测试模型混淆。
6. 与 MultiMapper 对接的主格式是精简 Clash-compatible YAML，并通过 `x-nekoray` 扩展携带订阅级 client、DoH、继承关系和来源标签；旧 JSON 仅作为历史兼容或无 YAML 构建 fallback。

## Git 与交付规则

1. 每轮可验证整改完成后应提交并推送，避免多轮迭代后状态混乱。
2. 提交前至少运行与改动范围匹配的构建或静态检查。
3. 交付说明必须区分“源码已修复”“增量构建已通过”“部署目录已覆盖”“zip 已生成”这几种状态。

## 持续推进规则

1. 每轮工作完成后必须规划下一步工作，直到 AnyTLS / sing-box 主线整改完全闭环。
2. 若仍有未完成项，交付说明不能只写“完成”，必须明确剩余风险、下一步涉及的模块、以及建议验证方式。
3. 发生中断、恢复或上下文压缩后，应从最新提交、规划文档和当前工作树继续推进，不能丢失已确认的开发原则。
4. 规划文档、契约文档和代码实现发生偏差时，应在同一轮工作中尽量收敛；无法一次完成时，必须把偏差列入下一步计划。
