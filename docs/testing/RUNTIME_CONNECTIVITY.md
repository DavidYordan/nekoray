# Windows 运行时连通性验证

状态：现行
最后更新：2026-07-22

## 环境硬约束

- `D:\Program Files\nekoray`、它监听的 `2080` 以及它维护的生产 TUN 不属于本项目，任何测试都不得停止、重启、改写或“临时接管”它们。
- 本项目默认 Mixed 端口是 `12080`。验证结果必须同时记录监听 PID 与可执行文件路径，不能用 `2080` 的成功代替本项目证据。
- 本机生产 TUN 会影响默认路由和出站归因。无法在不触碰生产网络的前提下得出结论时，转到 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md)，不得尝试自动关闭生产 TUN。

## 先分清两层语义

端口决定的是逻辑线路：主 Mixed `12080` 进入当前主 profile 的 `proxy` 链；每个辅助 Mixed 端口进入与该端口绑定的辅助 profile 链。当前生成器会在顶层 `custom_config` 合并前快照完整受管 Mixed listener 和沿 detour 可达的全部 outbound 对象，合并后要求对象逐项相同，并要求精确无条件绑定出现在可能改投或提前 resolve 的规则之前。profile 级 custom outbound 可在快照前修改普通字段，但不能新增/改变 detour。普通显式分流不得把 Mixed 流量改送 `direct`、`block` 或其它线路；该 validator 也不是对所有自定义路由/DNS 语义的全局证明，漏过时仍按 P0 回归处理。

`route.auto_detect_interface` 只让 sing-box 在操作系统路由层选择合格的底层接口，主要用于避免 TUN 回环。它不读取 Mixed 端口，也不在主线路和辅助线路之间做选择。测试报告必须分别记录“命中了哪个逻辑 outbound”和“底层套接字走了哪个接口”；不能用接口自动检测来修正端口映射。

## 三级验证

1. **本地无侵入验证**：配置导出与 `check`、direct fixture、Mixed HTTP/CONNECT/SOCKS5 contract、监听 PID 和端口映射。此级不得修改系统代理、TUN、路由或 DNS。
2. **OpenWrt 临时探针**：本机生产 TUN 使真实出站归因不清时，在 `192.168.1.7` 用相同版本 core 验证 schema、DNS、detour 和远端协议。具体边界见 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md)。
3. **Windows 集成验收**：Wintun、系统代理、WFP/fail-closed、GUI 生命周期、线路重启和 Windows 接口选择只能在 Windows 验证。优先使用独立 Windows 测试环境；只有必须停止本机生产 Nekoray 才能取得有效证据时，才申请用户安排维护窗口。

第二级成功只说明配置/出站链能在相同 core 上闭环；第二级成功而第三级失败，优先调查 Windows 路由、接口和生命周期。两级都以同一临时配置失败时，再调查节点、DNS、detour 或配置生成。

第二级的标准入口是先 dry-run、再真实运行；它固定使用 `127.0.0.1:52080`，并复核远端现有服务基线不变：

```powershell
& 'D:\complex\RouteFluent\.venv\Scripts\python.exe' `
  .\tools\verify_mixed_openwrt.py <exported-config.json> --dry-run --json

& 'D:\complex\RouteFluent\.venv\Scripts\python.exe' `
  .\tools\verify_mixed_openwrt.py <exported-config.json> --json
```

默认探针保留导出配置的 `auto_detect_interface`。`--force-auto-detect-interface` 只用于单独接口诊断，不能混入标准协议验收。

## 当前 L2 结论

2026-07-20 的历史 OpenWrt 对照中：主 `proxy` Mixed 能收到请求并命中目标 AnyTLS outbound；保持 `mihomo/1.19.28`、移除 `g-2` detour 后三种协议均返回 204。独立 profile 2 的 Trojan 也三协议返回 204，且结构比对确认它与 `g-2` 是同一个完整 outbound 对象。只有 “AnyTLS mihomo client + `g-2` detour” 主配置组合出现 HTTP 502、HTTPS 超时、SOCKS 空响应和 `failed to create session: EOF`；改用原生 AnyTLS client又触发服务端 internal error。该轮旧探针对所有临时变体强制 `auto_detect_interface=true`，只能在这一共同条件下隔离协议组合；需按当前默认 preserve 重跑后才能代表导出接口策略。

因此当前 “Mixed 无法连接” 表象不能再归因为入口解析器或端口映射完全失效，阻断项是主 outbound 组合链路。完整数据和安全基线见 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md#已验证基线)。这仍不是 Windows Wintun、WFP、系统代理或生命周期通过证明。

## 本地检查入口

整套 GUI/TUN 状态快照：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_runtime_connectivity.ps1 `
  -PackageDir <package-dir>
```

GUI 中勾选状态来自 requested `spmode_vpn`；文字中的 worker active/inactive 来自当前 worker 回报。两者都必须与脚本采集的 Windows adapter/route/DNS/PID 证据对照。core 崩溃后只会自动恢复空控制 core，不会恢复 profile/TUN；看到 **requested; inactive** 时不得继续执行依赖 TUN 的线路测试。

可用 `-ExpectedHttpStatus <code>` 指定精确期望状态。脚本会清空当前进程的 `NO_PROXY`，要求 curl 成功并核对状态码，也会验证目标端口的监听 PID 是否来自指定 package。它是快照，不会持续证明进程健康，也不能证明真实流量没有被生产 TUN 接管。

单个导出配置的隔离诊断：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_mixed_inbound.ps1 `
  -ConfigPath <runtime-config.json> -Json
```

脚本只操作临时副本，并只接受主 `mixed-in -> proxy` 连通性诊断；其它 `InboundTag` 会被拒绝，它不能验收辅助端口映射。脚本保留目标 loopback Mixed 和从 `proxy` 可达的精确 outbound detour 闭包，移除系统代理标记、其他 inbound、controller/service、无系统写入的 NTP 服务和文件日志；TUN、非空 top-level endpoints、`ntp.write_to_system=true` 与已占用端口会在启动前拒绝。它核对监听 PID，分别发起 HTTP absolute-form、HTTPS CONNECT 与 SOCKS5h 请求，最后只终止自己创建的精确 PID。认证通过 stdin 交给 curl，不进入进程命令行。

## 诊断开关

以下选项只修改临时副本：

```powershell
# 请求 sing-box 选择合格默认接口；不表示“自动选择线路”，也不保证绕过生产 TUN
.\tools\verify_mixed_inbound.ps1 -ConfigPath <config> -ForceAutoDetectInterface -Json

# 排除组级前置代理
.\tools\verify_mixed_inbound.ps1 -ConfigPath <config> -RemoveAnyTLSDetour -Json

# 对照 TLS 指纹
.\tools\verify_mixed_inbound.ps1 -ConfigPath <config> `
  -AnyTLSUtlsOverride chrome -Json
```

不要把“端口可连接”当成通过。至少要同时满足 `listener_owned_by_core`、HTTP、HTTPS CONNECT、SOCKS5h、`proxy_outbound_events`、无启动错误，并核对错误分类和目标逻辑 outbound。

## 已知限制

对真实节点直接运行 `verify_mixed_inbound.ps1` 仍依赖真实 DNS、节点和所选测试 URL，不能完全分离本地 Mixed contract 与远端出站。仓内 `test_mixed_probe.ps1` 与 `test_runtime_connectivity.ps1` 已改用精确 PID 持有的 loopback HTTP 204 origin，避免公共站抖动，并验证 origin 清理；它们仍未覆盖错误认证、主/辅助不同出口、WFP、IPv6 或持续健康，也不能证明进程就是本次 GUI 启动的实例。

2026-07-20 使用本轮 `build-package-windows64/nekobox_core.exe` 的回归中，Mixed fixture 为 7/7，额外 listener、系统代理、禁用日志和 origin 清理均通过；runtime connectivity 的 expected 204 正例中 HTTP/SOCKS5h 均为 204，expected 200 反例按预期报告 2 项 mismatch，系统代理、fixture 端口和 origin 清理均通过。这是 loopback/工具契约证据，不是生产节点或 Windows TUN/WFP 证据。

所有报告都应脱敏；其中可能包含节点名、服务器地址、路由、进程和本地路径。任何测试结果都不得以关闭或改写生产 Nekoray 为前提。
