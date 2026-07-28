# Windows 运行时连通性验证

状态：现行测试说明；线路断言已按产品契约更正
最后更新：2026-07-28

## 环境硬约束

- 本项目默认 Mixed 端口是 `2080`。验证结果必须同时记录监听 PID 与可执行文件路径，不能只凭端口成功认定属于本项目。
- 本机 Clash TUN 是外部底层网络，测试必须保持其运行，不得停止、重启或改写。
- Clash TUN 会影响默认路由、Fake-IP DNS 和出站归因。无法用进程级临时对照得出结论时，转到独立 Windows 环境或 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md)。

## 先分清两层语义

必须分开验证两类入口：原生 Mixed `2080` 保留 NekoRay 正常路由语义，当前主 profile 只是默认出站；新增专用 Mixed 端口才严格进入与其绑定的完整 chain。2026-07-28 的整改已移除主入口无条件终结绑定，并加入配置导出回归断言；主入口仍须继续对照 NekoRay 4.0.1 验证完整 route/reject/DNS 行为。提交 `3f7ff19` 又把普通规则中的显式 reject/block 编译为仅匹配对应辅助 inbound 的前置规则，随后才执行精确 chain 绑定；direct、bypass、主线和其它 outbound 不会被复制到该前置区。提交 `a3dee71` 已通过隔离 appdata 中的 ProfileManager/ConfigBuilder 生成一条两跳辅助 chain 并由当前 core `check`；`9a328a5` 又把双线路回环运行测试改为直接启动同一路径导出的两条单跳辅助线路。两跳 detour 仍只到 schema 层，真实节点与 Windows GUI 也未验证。

`route.auto_detect_interface` 只让 sing-box 在操作系统路由层选择合格的底层接口，主要用于避免 TUN 回环。它不读取 Mixed 端口，也不在主线路和辅助线路之间做选择。测试报告必须分别记录“命中了哪个逻辑 outbound”和“底层套接字走了哪个接口”；不能用接口自动检测来修正端口映射。

## 三级验证

1. **本地无侵入验证**：配置导出与 `check`、direct fixture、Mixed HTTP/CONNECT/SOCKS5 contract、监听 PID 和端口映射。此级不得修改系统代理、TUN、路由或 DNS。
2. **OpenWrt 临时探针**：本机 Clash TUN 使真实出站归因不清时，在 `192.168.1.7` 用相同版本 core 验证 schema、DNS、detour 和远端协议。具体边界见 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md)。
3. **Windows 集成验收**：真实 GUI、Mixed、系统代理/TUN 的上游回归、线路重启和 Windows 接口选择只能在 Windows 验证。WFP/persistent Runtime 不是当前核心发布门。优先使用独立 Windows 测试环境；只有必须停止 Clash TUN 才能取得有效证据时，才申请用户安排维护窗口。

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

2026-07-20 的历史 OpenWrt 对照中：临时 `52080` Mixed 被显式重写为命中目标 AnyTLS outbound；保持 `mihomo/1.19.28`、移除 `g-2` detour 后三种协议均返回 204。独立 profile 2 的 Trojan 也三协议返回 204，且结构比对确认它与 `g-2` 是同一个完整 outbound 对象。只有 “AnyTLS mihomo client + `g-2` detour” 目标链组合出现 HTTP 502、HTTPS 超时、SOCKS 空响应和 `failed to create session: EOF`；改用原生 AnyTLS client 又触发服务端 internal error。该轮旧探针对所有临时变体强制 `auto_detect_interface=true`，只能在这一共同条件下隔离协议组合；需按当前默认 preserve 重跑后才能代表导出接口策略。

因此历史证据只说明临时 `52080` Mixed 解析器能工作，且故障可在该临时映射下收敛到 AnyTLS 与 Trojan detour 的组合；它不能排除产品 `2080`/辅助端口映射问题，也不能把“主 outbound 组合”写成唯一剩余阻断项。完整数据和安全基线见 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md#已验证基线)。这仍不是 Windows Wintun、WFP、系统代理或生命周期通过证明。

## 本地检查入口

整套 GUI/TUN 状态快照：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_runtime_connectivity.ps1 `
  -PackageDir <package-dir>
```

GUI 中勾选状态来自 requested `spmode_vpn`；文字中的 worker active/inactive 来自当前 worker 回报。两者都必须与脚本采集的 Windows adapter/route/DNS/PID 证据对照。core 崩溃后只会自动恢复空控制 core，不会恢复 profile/TUN；看到 **requested; inactive** 时不得继续执行依赖 TUN 的线路测试。

可用 `-ExpectedHttpStatus <code>` 指定精确期望状态。脚本会清空当前进程的 `NO_PROXY`，要求 curl 成功并核对状态码，也会验证目标端口的监听 PID 是否来自指定 package。它是快照，不会持续证明进程健康，也不能证明真实流量没有被 Clash TUN 接管。

单个导出配置的隔离诊断：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_mixed_inbound.ps1 `
  -ConfigPath <runtime-config.json> -Json
```

脚本只操作临时副本，并只接受主 `mixed-in -> proxy` 连通性诊断；其它 `InboundTag` 会被拒绝，它不能验收辅助端口映射。脚本保留目标 loopback Mixed 和从 `proxy` 可达的精确 outbound detour 闭包，移除系统代理标记、其他 inbound、controller/service、无系统写入的 NTP 服务和文件日志；TUN、非空 top-level endpoints、`ntp.write_to_system=true` 与已占用端口会在启动前拒绝。它核对监听 PID，分别发起 HTTP absolute-form、HTTPS CONNECT 与 SOCKS5h 请求，最后只终止自己创建的精确 PID。认证通过 stdin 交给 curl，不进入进程命令行。

专用端口规则的双回环运行测试：

```powershell
.\test\test_auxiliary_route_runtime.ps1 `
  -ExecutablePath .\build-package-windows64\nekobox.exe `
  -CorePath .\deployment\windows64\nekobox_core.exe -Json
```

该测试在受保护临时目录中初始化隔离 appdata，持久化主 HTTP profile、两个辅助 HTTP profile、辅助端口映射和 reject/跨线 route，再经 GUI 审计导出、core `check` 后启动同一 JSON。主 Mixed 为避免碰撞只在该隔离数据中改到 `18119`；默认 `2080` 仍由配置 guard 单独断言，测试不会改产品默认。运行阶段验证端口 A/B 分别命中返回 210/211 的回环上游、terminal 后的 `bypass` 规则不能移动 A、显式 reject 不触达任一上游，以及停止 A 上游后 B 与主/辅三个 listener 仍工作。它核对精确 PID、要求生成配置无 TUN、系统代理请求或 `auto_detect_interface`，比较系统代理前后快照，只结束自己创建的进程并删除临时 appdata/config。它不代表真实节点或两跳 detour 可用。

最终生成链路的无启动审计：

```powershell
.\test\test_final_config_guards.ps1 `
  -ExecutablePath .\build-package-windows64\nekobox.exe `
  -CorePath .\deployment\windows64\nekobox_core.exe
```

其中辅助线路用例在隔离 appdata 中持久化主 SOCKS profile、辅助 chain profile 和两个文档保留地址的 SOCKS hop，再调用显式辅助审计导出。它要求普通 `2080` route 仍命中原生规则，辅助 listener 精确指向两跳 detour 闭包，reject 位于 terminal 前而跨线 redirect 位于 terminal 后；同时要求导出不含 TUN、`set_system_proxy=true` 或 `auto_detect_interface`。普通导出仍必须省略辅助线路，`for_test` 与辅助审计组合必须失败。该用例只写临时文件并执行 `check`；两跳 chain 的实际启动仍未覆盖。

## 诊断开关

以下选项只修改临时副本：

```powershell
# 请求 sing-box 选择合格默认接口；不表示“自动选择线路”，也不保证完整绕过 Clash Fake-IP
.\tools\verify_mixed_inbound.ps1 -ConfigPath <config> -ForceAutoDetectInterface -Json

# 排除组级前置代理
.\tools\verify_mixed_inbound.ps1 -ConfigPath <config> -RemoveAnyTLSDetour -Json

# 对照 TLS 指纹
.\tools\verify_mixed_inbound.ps1 -ConfigPath <config> `
  -AnyTLSUtlsOverride chrome -Json
```

不要把“端口可连接”当成通过。至少要同时满足 `listener_owned_by_core`、HTTP、HTTPS CONNECT、SOCKS5h、`proxy_outbound_events`、无启动错误，并核对错误分类和目标逻辑 outbound。

## 已知限制

对真实节点直接运行 `verify_mixed_inbound.ps1` 仍依赖真实 DNS、节点和所选测试 URL，不能完全分离本地 Mixed contract 与远端出站。仓内 `test_mixed_probe.ps1` 与 `test_runtime_connectivity.ps1` 已改用精确 PID 持有的 loopback HTTP 204 origin，避免公共站抖动，并验证 origin 清理；`test_auxiliary_route_runtime.ps1` 现覆盖 ProfileManager/ConfigBuilder 生成的两个专用端口、不同回环出口、reject 和单上游故障隔离，配置导出 guard 另覆盖两跳 chain 结构。仍未覆盖生成两跳 detour 的实际运行、错误认证、真实节点 profile、WFP、IPv6 或持续健康，也不能证明普通 GUI 生命周期已完成同一闭环。

2026-07-22 使用本轮 `deployment/windows64/nekobox_core.exe` 的回归中，Mixed fixture 为 7/7，额外 listener、系统代理、禁用日志和 origin 清理均通过；runtime connectivity 的 expected 204 正例中 HTTP/SOCKS5h 均为 204，expected 200 反例按预期报告 2 项 mismatch，系统代理、fixture 端口和 origin 清理均通过。clean GUI build tree 不输出 `nekobox_core.exe`。这是 loopback/工具契约证据，不是生产节点或 Windows TUN/WFP 证据。

所有报告都应脱敏；其中可能包含节点名、服务器地址、路由、进程和本地路径。任何测试结果都不得以关闭或改写 Clash TUN 为前提。
