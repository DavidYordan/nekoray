# profile core 配置导出

状态：现行
最后更新：2026-07-20

该工具用于查看 GUI 对指定 profile 实际生成的 sing-box 配置，避免只根据数据库字段或 UI 文案推断运行行为。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\export_profile_core_config.ps1 `
  -ProfileId <profile-id> -OutputPath <output.json> -Check
```

可选模式：

- 不加模式：默认就是安全审计导出；不启动线路，省略产品 TUN、辅助 Mixed 运行态及相关本机字段，并执行已知 OS 副作用校验。
- `-ForTest`：生成 URL/full-test 使用的有界配置；`internal-full` 被拒绝，顶层 `custom_config` 只要改变生成结果就失败。
- `-ForShare`：兼容别名，当前与默认导出走同一个 `forExport` 路径。
- `-Check`：调用同目录 core 的 `check`，只证明 schema/启动前配置有效。

## 安全边界

- 导出文件可能包含服务器、认证材料和 DNS/路由信息，默认不得提交 Git。
- `check` 成功不代表 Mixed 可访问、DNS 可用或远端握手成功；连通性应继续使用 `verify_mixed_inbound.ps1`。
- `nekobox_core check/run` 是直接读取 sing-box 配置的高级 CLI，不会重复 C++ ConfigBuilder 的产品策略；本工具之所以可用，是因为配置先由 GUI 导出路径生成并通过其 guard。不得把任意外部 JSON 的 `check` 成功解释为安全产品配置。
- “安全导出”只表示导出动作本身不启动线路，且构建器剥离/拒绝已知 OS 模式副作用；文件仍可能含主 Mixed、自定义 listener 和完整网络配置，不得未经审计直接启动。
- 首次对真实配置目录运行任何 GUI 工具前先完整备份 `config/`；接管工作树会保留未知 profile，但尚未提供 quarantine/UI 恢复与完整迁移验收。
- 调试结束后安全删除包含凭据的临时导出文件。

部分最终配置拒绝分支可在隔离临时 appdata 中验证：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\test\test_final_config_guards.ps1 `
  -ExecutablePath <package-dir>\nekobox.exe
```

该脚本当前覆盖安全 `internal-full` 文件导出；未请求 TUN、`set_system_proxy`、系统 WireGuard/Tailscale endpoint 和 NTP 写系统时钟的拒绝；以及标准 SOCKS profile 的测试态顶层 custom 拒绝、Managed Mixed listener 精确快照、profile 级 detour 禁改和普通 custom route 不产生 `rules:null`/`outbound:null`。2026-07-20 接管回归为 10/10；它仍不是完整 C++ golden 测试，也不启动 TUN 或修改系统网络。
