# Windows 运行时连通性验证

状态：现行测试说明；线路断言已按产品契约更正
最后更新：2026-08-31

## 环境硬约束

- 本项目默认 Mixed 端口是 `2080`。验证结果必须同时记录监听 PID 与可执行文件路径，不能只凭端口成功认定属于本项目。
- 本机 Clash TUN 是不可中断的外部底层网络，测试必须保持其运行；不得停止、重启、结束其进程，也不得改写配置、接口、DNS 或路由。当前主机不存在“停 Clash 维护窗口”这条验证路径。
- Clash TUN 会影响默认路由、Fake-IP DNS 和出站归因。无法用进程级临时对照得出结论时，停止本机实验并转到独立 Windows 环境、WSL 或 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md)，同时遵守各环境的证据边界。

## 先分清两层语义

必须分开验证两类入口：原生 Mixed `2080` 保留 NekoRay 正常路由语义，当前主 profile 只是默认出站；新增专用 Mixed 端口才严格进入与其绑定的完整 chain。2026-07-28 的整改已移除主入口无条件终结绑定，并加入配置导出回归断言；主入口仍须继续对照 NekoRay 4.0.1 验证完整 route/reject/DNS 行为。提交 `3f7ff19` 又把普通规则中的显式 reject/block 编译为仅匹配对应辅助 inbound 的前置规则，随后才执行精确 chain 绑定；direct、bypass、主线和其它 outbound 不会被复制到该前置区。提交 `a3dee71` 已通过隔离 appdata 中的 ProfileManager/ConfigBuilder 生成一条两跳辅助 chain 并由当前 core `check`；`9a328a5` 把双线路回环运行改为启动同一路径导出的配置；`55bb799` 以 group `front_proxy_id` 生成并实际运行 A 两跳/B 单跳；`f298d46` 再将 A 替换为实际 Mihomo-client AnyTLS + Trojan detour。显式 chain profile、真实供应商节点与 Windows GUI 仍未运行验证。

`route.auto_detect_interface` 只让 sing-box 在操作系统路由层选择合格的底层接口，主要用于避免 TUN 回环。它不读取 Mixed 端口，也不在主线路和辅助线路之间做选择。测试报告必须分别记录“命中了哪个逻辑 outbound”和“底层套接字走了哪个接口”；不能用接口自动检测来修正端口映射。

## 分层验证

1. **本地无侵入验证**：配置导出与 `check`、direct fixture、Mixed HTTP/CONNECT/SOCKS5 contract、监听 PID 和端口映射。此级不得修改系统代理、TUN、路由或 DNS。
2. **WSL/Linux 隔离验证**：可验证 Linux core、配置、schema、协议 loopback 和无 Windows 副作用的故障路径。WSL2 仍借宿主网络出站，不能据此证明物理出站归属，也不能替代 Windows Wintun、系统代理、WFP、驱动和 GUI 生命周期。
3. **OpenWrt 临时探针**：本机 Clash TUN 使真实出站归因不清时，在授权且可达的隔离实验机用相同版本 core 验证 schema、DNS、detour 和远端协议。具体边界见 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md)。
4. **独立 Windows 集成验收**：真实 GUI、Mixed、系统代理/TUN 的上游回归、线路重启和 Windows 接口选择只能在 Windows 验证。WFP/persistent Runtime 不是当前核心发布门。应使用带快照和精确资源所有权的独立 Windows VM、Windows 沙盒或其它测试机；不得为取得证据停止或改写本机 Clash。

WSL/OpenWrt 成功只说明相应 core/配置/出站链在该环境闭环；独立 Windows 失败时，优先调查 Windows 路由、接口和生命周期。各环境以同一脱敏临时配置失败时，再调查节点、DNS、detour 或配置生成。任何低层成功都不能上推为未执行层级已经通过。

OpenWrt 层的标准入口是先 dry-run、再真实运行；它固定使用 `127.0.0.1:52080`，并复核远端现有服务基线不变：

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

该测试在受保护临时目录中初始化隔离 appdata，持久化主 HTTP profile、A AnyTLS terminal、A Trojan group front proxy、B HTTP 单跳 profile、辅助端口映射和 reject/跨线 route，再经 GUI 审计导出、core `check` 后启动同一 JSON。主 Mixed 为避免碰撞只在该隔离数据中改到 `18119`；默认 `2080` 仍由配置 guard 单独断言，测试不会改产品默认。测试每次生成只含 `localhost`/`127.0.0.1` 的临时自签证书；当前 core 分别启动独立 Trojan inbound、AnyTLS inbound 和产品 client，Python 桩只承担 A HTTP origin 与 B HTTP proxy。运行阶段要求 A 以 `client=mihomo/1.19.28` 经 Trojan detour 到 origin 并返回 210，B 返回 211，terminal 后的 `bypass` 不能移动 A，显式 reject 不触达两个 HTTP 目标；停止 Trojan 并确认 AnyTLS server 与 origin 仍由原 PID 监听后，A 必须失败而 B 与主/辅三个 listener 继续工作。测试要求所有三份 core 配置通过 `check`，生成配置无 TUN、系统代理请求或 `auto_detect_interface`，比较系统代理前后快照，只结束自己创建的进程并释放七个临时端口。它不代表真实供应商节点、显式 chain profile 或普通 GUI 可用。

最终生成链路的无启动审计：

```powershell
.\test\test_final_config_guards.ps1 `
  -ExecutablePath .\build-package-windows64\nekobox.exe `
  -CorePath .\deployment\windows64\nekobox_core.exe
```

其中辅助线路用例在隔离 appdata 中持久化主 SOCKS profile、辅助 chain profile 和两个文档保留地址的 SOCKS hop，再调用显式辅助审计导出。它要求普通 `2080` route 仍命中原生规则，辅助 listener 精确指向两跳 detour 闭包，reject 位于 terminal 前而跨线 redirect 位于 terminal 后；同时要求导出不含 TUN、`set_system_proxy=true` 或 `auto_detect_interface`。普通导出仍必须省略辅助线路，`for_test` 与辅助审计组合必须失败。该用例只写临时文件并执行 `check`；另一个运行用例已覆盖 group front proxy 两跳，但没有启动这个显式 chain profile。

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

对真实节点直接运行 `verify_mixed_inbound.ps1` 仍依赖真实 DNS、节点和所选测试 URL，不能完全分离本地 Mixed contract 与远端出站。仓内 `test_mixed_probe.ps1` 与 `test_runtime_connectivity.ps1` 已改用精确 PID 持有的 loopback HTTP 204 origin，避免公共站抖动，并验证 origin 清理；`test_auxiliary_route_runtime.ps1` 现覆盖 ProfileManager/ConfigBuilder 生成的两个专用端口、A AnyTLS + Trojan group front proxy、B HTTP 单跳、reject、前置代理故障隔离和不绕过 detour，配置导出 guard 另覆盖显式 chain profile 结构。仍未覆盖显式 chain profile 运行、错误认证、真实供应商节点、WFP、IPv6 或持续健康，也不能证明普通 GUI 生命周期已完成同一闭环。

2026-07-22 使用本轮 `deployment/windows64/nekobox_core.exe` 的回归中，Mixed fixture 为 7/7，额外 listener、系统代理、禁用日志和 origin 清理均通过；runtime connectivity 的 expected 204 正例中 HTTP/SOCKS5h 均为 204，expected 200 反例按预期报告 2 项 mismatch，系统代理、fixture 端口和 origin 清理均通过。clean GUI build tree 不输出 `nekobox_core.exe`。这是 loopback/工具契约证据，不是生产节点或 Windows TUN/WFP 证据。

所有报告都应脱敏；其中可能包含节点名、服务器地址、路由、进程和本地路径。任何测试结果都不得以关闭或改写 Clash TUN 为前提。
