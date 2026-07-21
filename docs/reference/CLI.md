# 命令行参数

状态：现行
最后更新：2026-07-20

## 用户参数

- `-many`：忽略同目录单实例保护。当前属于 legacy/unsafe；只能用于不接管系统代理/TUN的隔离配置导出，正式运行和系统模式测试禁止使用。
- `-appdata [dir]`：使用 Windows 应用配置目录，或使用显式 `dir` 保存配置。
- `-tray`：启动后不显示主窗口。
- `-debug`：启用调试模式并向 core 传递 `-debug`。

## 内部重启参数

- `-flag_restart_profile_id <id>`

`-flag_restart_tun_on` 已停止解析和生成，只在清洗 legacy argv 时移除。非管理员进程不会自动提权重启或自动启用 TUN；用户必须自行以管理员身份启动，然后再次手动启用。`-flag_restart_profile_id` 依赖尚未完成的运行时所有权交接，不能作为安全重启机制。

core 意外退出且 TUN 仍是 requested state 时，当前实现只重启空控制 core，不通过 `-flag_restart_profile_id` 自动恢复 profile/TUN。UI 会显示 requested 但 worker inactive；这不等于 Windows fail-closed 已实现。

历史 `-flag_restart_system_proxy_on` 已停止解析/生成，只在清洗旧 argv 时移除；系统代理绝不在启动时自动恢复。

## 维护参数

- `-flag_reorder`：旧实现会先删后写，接管工作树已将该参数改为明确警告并保持零文件变化；完成事务式迁移工具前不可用。

## core 配置导出

```powershell
nekobox.exe -flag_export_profile_config <profile-id> <output-json>
```

可追加：

- `-flag_export_profile_config_for_test`：只生成 URL/full-test 使用的有界配置；拒绝 `internal-full`，且顶层 `custom_config` 只要改变生成结果就失败。
- `-flag_export_profile_config_for_share`：兼容别名；当前与默认导出使用同一 `forExport` 安全路径。

默认导出、`for_share` 和 `for_test` 都不会启动线路，并执行已知 OS 副作用校验。默认/分享导出会省略产品 TUN、辅助 Mixed 运行态及相关本机字段；它仍可包含主 Mixed、自定义 listener、server、密码、DoH 和路由，因此只适合审计/分享前脱敏，不能当作可盲目启动的沙箱配置。

## core 高级 CLI

`nekobox_core.exe` 还有三个显式模式：

```powershell
nekobox_core.exe version
nekobox_core.exe check -c <config.json>
nekobox_core.exe run -c <config.json>
```

- `check` 解析配置并执行 sing-box pre-start 校验；它不是纯 JSON parser，也不证明远端连通、Windows OS 副作用安全或本项目产品策略通过。只对已生成/收紧的临时配置使用，不要拿任意配置试运行式“检查”。
- `run` 直接启动给定 sing-box 配置。仓内构建和隔离测试工具依赖这一入口，但都会先生成或收紧临时副本并核对精确 PID；它不是普通 GUI 操作，也不得用于未经审计的配置。
- 两者还接受 sing-box 兼容的 `-C/--config-directory` 和 `-D/--directory`。正式产品流程不得用目录合并或工作目录切换规避 ConfigBuilder。

普通 GUI 使用的是 `nekobox` 内部模式：core 只监听 `127.0.0.1`，GUI 每次启动生成随机 token，并经 stdin 传入；每个 gRPC 请求都验证 token。该事实意味着普通 GUI 不是无认证任意入口，但不构成完整安全边界：Go `Start` 会执行 sing-box 的配置与启动流程，却尚未重复 C++ ConfigBuilder 的 Managed Mixed、TUN、系统代理和 provider resolver 策略校验。后续应在受控 core/Runtime 中补第二层校验，不能把随机 token 或 `check` 成功当作配置授权。
