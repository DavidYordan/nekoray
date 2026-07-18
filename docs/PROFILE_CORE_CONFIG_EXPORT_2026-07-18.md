# Profile 最终 core 配置导出

本项目新增了一个诊断入口，用于把某条线路最终传给 sing-box core 的 JSON 配置导出到文件。

用途：

- 确认订阅级默认 `client`、server resolver DoH、链式前置代理、辅助端口等是否真实进入最终运行配置。
- 对比“数据库里的 profile 字段”和“core 实际收到的配置”。
- 用 `nekobox_core.exe check -c` 校验配置格式，而不启动真实线路。

命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\export_profile_core_config.ps1 -ProfileId 66 -Check
```

默认读取 `deployment/windows64/config`，输出到 `deployment/windows64/runtime_audit/profile_<id>_core_config_<timestamp>.json`。

边界：

- 不启动线路，不监听 mixed 端口，不修改系统代理，不修改 TUN。
- 不关闭或影响 `D:\Program Files\nekoray` 生产实例。
- 只在显式传入 `-flag_export_profile_config` 时触发，正常 UI 启动路径不变。
- 导出的 JSON 会包含真实 server、password、DoH 等敏感信息，不应放入 git 或对外发送。

直接调用 GUI 入口：

```powershell
.\deployment\windows64\nekobox.exe -flag_export_profile_config 66 .\runtime_audit\profile_66_core_config.json
```

可选模式：

- 默认模式：导出真实启动时使用的运行配置。
- `-ForTest`：导出 URL 测试配置，对应 GUI 测速使用的 `BuildConfig(forTest=true)`。
- `-ForShare`：导出分享/复制用途配置，对应 `BuildConfig(forExport=true)`。

当前 AnyTLS 排查原则：

1. 先导出运行配置并确认 `proxy` outbound 是否为 `type=anytls`。
2. 确认 Clash 订阅导入的 AnyTLS 是否带 `client=mihomo/1.19.28`。
3. 确认 AnyTLS server 域名是否绑定 `domain_resolver`，且 DNS servers 中有 `routefluent_resolver_group` 和订阅 DoH。
4. 用 `nekobox_core.exe check -c` 先排除配置 schema 错误。
5. 若 Trojan 基线可用、AnyTLS 配置校验通过但运行时仍 `failed to create session: EOF`，则优先怀疑 AnyTLS 线路参数、服务端策略、服务端兼容性或该节点当前不可用，而不是本地入站端口或生产 TUN 抢流。
