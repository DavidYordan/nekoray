# Windows 运行态连通性验证

本文记录本项目在 Windows 上验证主端口、辅助端口、Clash API 和系统代理指向的方法。

脚本：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_runtime_connectivity.ps1
```

默认行为：

- 只读检查，不启动或停止 `nekobox.exe`。
- 不修改 Windows 系统代理、路由、TUN 或项目配置。
- 只检查 `deployment/windows64` 下的项目实例，不触碰 `D:\Program Files\nekoray` 生产实例。
- 从 `deployment/windows64/config/groups/nekobox.json` 读取主 mixed 端口、Clash API 端口、测试 URL 和持久化辅助端口。
- 报告写入 `deployment/windows64/runtime_audit/runtime_<timestamp>.json`。

## 基线检查

未启动项目线路时也可以运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_runtime_connectivity.ps1 -SkipCurl
```

这会验证脚本本身和配置读取路径。若主端口未监听、没有项目进程或没有辅助端口，脚本只给 `Warnings`，不会把它当成代码失败。

## 真实运行态检查

1. 启动 `deployment/windows64/nekobox.exe`。
2. 启动一条主线路。
3. 如需验证多线路，右键另一条线路执行 `Start auxiliary port`。
4. 运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_runtime_connectivity.ps1 -ExpectRunning
```

脚本会检查：

- 主 mixed 端口是否监听。
- 每个持久化辅助端口是否监听。
- 使用 `curl.exe` 分别通过 `socks5h://127.0.0.1:<port>` 和 `http://127.0.0.1:<port>` 请求测试 URL。
- 如果 Clash API 已启用，检查 API 端口并请求 `/version`。
- Windows 当前用户系统代理是否开启、指向哪里。

需要强制要求系统代理指向本项目主端口时：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_runtime_connectivity.ps1 -ExpectRunning -ExpectSystemProxy
```

需要强制要求至少存在一个辅助端口时：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_runtime_connectivity.ps1 -ExpectRunning -ExpectAuxiliary
```

## 常用参数

- `-PackageDir <path>`：指定部署目录，默认 `deployment/windows64`。
- `-TestUrl <url>`：指定 curl 测试 URL；默认读取项目配置里的 `test_url`，再回退到 `http://cp.cloudflare.com/`。
- `-TimeoutSeconds 10`：curl 请求超时时间。
- `-ExpectRunning`：主端口未监听时判定失败。
- `-ExpectSystemProxy`：Windows 系统代理未指向主端口时判定失败。
- `-ExpectAuxiliary`：没有持久化辅助端口或辅助端口未监听时判定失败。
- `-SkipCurl`：只检查配置、进程、端口监听，不发起网络请求。
- `-Json`：仅输出 JSON，便于自动化消费。
- `-OutputPath <path>`：指定报告文件。

## 与无偷跑验证的关系

`verify_runtime_connectivity.ps1` 只回答“当前端口和线路是否可用”。

`verify_fail_closed_restart.ps1` 回答“重启或重载期间是否把系统代理/TUN 错误恢复成直连”。

实际验收建议先运行连通性脚本确认主端口、辅助端口和 Clash API 正常，再用无偷跑脚本监控 `Restart Proxy`、`Restart Program`、辅助端口变更和更新器重启窗口。
