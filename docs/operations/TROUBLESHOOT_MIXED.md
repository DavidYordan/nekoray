# Mixed 入口排障

状态：现行
最后更新：2026-07-24

Mixed 同时接受 HTTP proxy 和 SOCKS5。排障必须区分“监听/入口协议”“逻辑线路映射”“DNS”“远端 outbound”和“底层接口”五层。

## 0. 先保护底层网络

本项目默认 Mixed 端口是 `2080`。本机 Clash TUN 是外部底层网络，必须保持运行；禁止为了排障擅自停止、重启或改写它，也不得把 Clash 的接口/Fake-IP 特例硬编码进产品配置。详见 [Clash TUN 共存](CLASH_TUN_COEXISTENCE.md)。

如果 Clash TUN 使本机出站归因不清，优先使用进程级临时对照、独立 Windows 环境或 [OpenWrt 远程实验室](../testing/OPENWRT_REMOTE_LAB.md)。不要为了完成排障自动关闭 Clash TUN。

## 1. 确认进程与监听者

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -in @('nekobox.exe','nekoray.exe','nekobox_core.exe') } |
  Select-Object ProcessId, Name, ExecutablePath, CommandLine

Get-NetTCPConnection -State Listen -LocalPort 2080 |
  Select-Object LocalAddress, LocalPort, OwningProcess
```

结果必须回查 PID 的 `ExecutablePath`。`2080` 未监听则先查本项目是否启动、配置 schema、端口占用和 core 日志；`2080` 可连但 PID/路径不属于当前 package，也不能作为本项目成功证据。

GUI 与 `nekobox_core.exe` 必须来自同一轮构建。每次 core 启动后，日志中的 `grpc server listening` 只会触发控制面探测；只有出现 `Core RPC identity handshake accepted for daemon generation ...` 才表示 GUI 已核对 UUID 和 lifecycle protocol version 3 并允许发送 Start。握手失败会明确记录 bounded retry 后仍 unavailable，v1/v2 或其它旧 core 与 v3 GUI/core 组合不会兼容回退。即便握手成功，也只表示精确控制 daemon 可用，不表示 profile 已启动、`2080` 已监听或线路/TUN/WFP 健康。

配置端口位于 `<package-dir>/config/groups/nekobox.json`。UI 显示的 `Mixed: 地址:端口` 只是配置值，不是健康状态。

## 2. 核对端口到逻辑线路

- 主 Mixed `2080` 应进入当前主 profile 构建出的 `proxy` 链。
- 每个辅助 Mixed 端口应进入与该端口绑定的辅助 profile 链。
- 当前生成器在顶层 `custom_config` 合并前捕获完整受管 Mixed listener 和沿 detour 可达的每个 outbound 对象；合并后要求对象逐项一致、tag/port 唯一，并拒绝精确 terminal binding 前的改投/提前 resolve、direct/block/selector/urltest 目标及缺失/循环 detour。profile 级 custom outbound 可在捕获前修改普通字段，但不能新增/改变 detour。若仍观察到改道，应保留最终配置和构建错误作为回归缺陷；该 validator 也不能被外推为所有自定义路由/DNS 已经安全。
- `route.auto_detect_interface` 只决定 outbound 套接字使用哪个底层 OS 接口，不负责按端口选择主/辅助线路。

因此，“2080 命中哪个 outbound”和“该 outbound 最后选择哪个网卡”必须分别取证。

## 3. 分别测试入口协议

```powershell
curl.exe --proxy http://127.0.0.1:2080 `
  --max-time 15 -o NUL -w "%{http_code}`n" `
  http://cp.cloudflare.com/

curl.exe --proxy http://127.0.0.1:2080 `
  --max-time 15 -o NUL -w "%{http_code}`n" `
  https://cp.cloudflare.com/

curl.exe --proxy socks5h://127.0.0.1:2080 `
  --max-time 15 -o NUL -w "%{http_code}`n" `
  http://cp.cloudflare.com/
```

使用入口认证时必须提供账号密码，但不得把凭据写入共享日志或进程命令行。本机成功只说明整条请求完成；Clash TUN 存在时，不能单凭该结果证明底层物理接口或排除代理套代理。

## 4. 用导出配置做本地隔离

默认导出不会启动线路，会省略产品 TUN 和辅助运行态并执行已知 OS 副作用校验；它仍可能包含凭据、主 Mixed 和自定义 listener，不能未经审计直接启动：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\export_profile_core_config.ps1 `
  -ProfileId <profile-id> -Check
```

随后启动安全收紧的临时探针：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\verify_mixed_inbound.ps1 `
  -ConfigPath <exported-config.json> -Json
```

脚本只支持主 `mixed-in -> proxy` 连通性诊断，传入其它 `InboundTag` 会拒绝；它不是辅助端口映射 contract 测试器。脚本只保留目标 loopback Mixed 和从 `proxy` 可达的精确 outbound detour 闭包，拒绝 TUN、非空 top-level endpoints、`ntp.write_to_system=true` 与占用端口，并移除无系统写入的 NTP 服务。它核对监听 PID，并只结束自己创建的精确 PID。`CorePath`、配置和临时路径规范化后必须解析为本地固定磁盘的盘符路径；共享护栏拒绝生产根、UNC/设备路径、ADS/额外冒号命名空间、网络映射或可移动盘、SUBST/DOS 设备重定向、8.3 短路径与 ReparsePoint/junction，并会比对与 `D:` 指向同一物理卷的盘符别名。诊断选项只修改临时副本：

这个护栏不通过最终文件句柄验证 final-file identity，所以不能识别所有已存在 hardlink。不要把另一路径下的同文件当作隔离副本；对可执行文件、输入配置和临时根仍需核对实际来源。

```powershell
# 底层接口对照；不是逻辑线路自动选择，也不保证完整绕过 Clash Fake-IP
... -ForceAutoDetectInterface

# 排除 group front proxy
... -RemoveAnyTLSDetour

# 对照 AnyTLS client / ALPN / uTLS
... -AnyTLSClientOverride native
... -AnyTLSAlpnOverride h2-http1
... -AnyTLSUtlsOverride chrome
```

如果本机 Clash TUN 使底层出站无法归因，用同一个导出配置执行 L2 对照；先 dry-run，不得跳过：

```powershell
& 'D:\complex\RouteFluent\.venv\Scripts\python.exe' `
  .\tools\verify_mixed_openwrt.py <exported-config.json> --dry-run --json

& 'D:\complex\RouteFluent\.venv\Scripts\python.exe' `
  .\tools\verify_mixed_openwrt.py <exported-config.json> --json
```

真实探针必须看到 `baseline_unchanged=true` 和 `remote_directory_cleaned=true`；否则首先处理安全/清理失败，不继续把联网结果解释成产品结论。

## 5. 按症状定位

- `listener_owned_by_core=false`：检查端口、schema、core 启动和占用冲突。
- 三种协议都能进入 Mixed、但全部远端失败：继续查 DNS、TCP、TLS、detour 和节点；不要先改入口。
- HTTP absolute-form 成功但 HTTPS CONNECT 失败：重点检查 CONNECT 隧道和远端握手。
- Trojan 成功而 AnyTLS EOF/超时：入口大概率正常，转查 AnyTLS、front proxy、uTLS/ALPN 和节点状态。
- AnyTLS mihomo client 去掉 detour 后成功、detour 对象单独也成功，但组合出现 `failed to create session: EOF`：把问题定位为组合链路或其生成/运行语义，不要再归为 Mixed parser 故障。
- 原生 AnyTLS client 收到服务端 internal error、mihomo client 成功：说明 client 兼容语义有实际作用，不要通过删除 client 字段“修复”detour 问题。
- `resolver_unavailable>0`：先看订阅来源。专用 `proxy-server-nameserver` 显式存在时只检查它，不能借普通 nameserver；专用字段 absent 时再检查 `dns.nameserver` 中的 HTTPS DoH。随后检查 provider DoH 可达性和 `dns-local` 是否只用于 endpoint bootstrap；不得让 provider DoH 失败后通过本机 DNS、direct 或其它线路 fallback 掩盖故障。
- 出现 `obsolete import policy`：这是旧组里非空 resolver 值的迁移护栏，不要手工复制旧值；成功刷新该订阅后，来源、版本和 DoH 列表会原子地随 group 保存。若刷新失败，旧线路数据保持不变。
- 只有本机失败、OpenWrt 相同临时配置成功：优先检查 Windows 默认路由、TUN 回环、接口选择和进程生命周期。
- 本机与 OpenWrt 同一配置均失败：优先检查配置生成、DNS、detour、节点或上游网络。
- GUI 退出时提示正在等待某个精确 core PID：不要重复关闭、kill 该进程或手工替换 core。结构化 Exit ACK 已提交，或 ACK 丢失后无法证明未接纳时，continuation fence 会持续等待同一 `{generation, UUID, PID}` finished；这不是 Mixed 入口自动检测或线路 fallback。只有退出详情明确说明对账已证明 clean non-admission（协议条件为 `STOPPED/FENCED_NOT_ADMITTED`）时，GUI 才能安全恢复控制。

2026-07-20 的历史 OpenWrt 真实对照中：保持主配置时，Mixed 和目标 outbound 均有事件，但 “AnyTLS `mihomo/1.19.28` + `g-2` detour” 返回 HTTP 502、HTTPS 超时、SOCKS 空响应；移除 detour 后三协议均为 204，独立 Trojan/profile 2 与独立 `g-2` 完整 outbound 也均为 204。该轮旧探针对所有临时变体强制 `auto_detect_interface=true`，只能证明共同接口变体下存在组合差异，不能证明产品导出接口策略；必须按当前默认 preserve 重跑。详细证据和安全基线见 [OpenWrt 已验证基线](../testing/OPENWRT_REMOTE_LAB.md#已验证基线)。这也不证明 Windows 集成已经通过。

早期本机接管实验已归档到 [2026-07-20 接管基线](../archive/audits/2026-07-20-takeover-baseline.md)，不得用其中单次成功覆盖新的可重复 OpenWrt 对照。

## 6. 何时需要 Windows 维护窗口

OpenWrt 能验证 schema、Mixed、Trojan/AnyTLS、DNS 和 detour，不能验证 Wintun、系统代理、WFP、GUI 退出/重启或线路热重启。先在独立 Windows 测试环境验证这些行为；只有 Windows 专有问题无法在独立环境复现，且 Clash TUN 的接口或路由使本机证据无效时，才请求用户安排维护窗口。agent 和测试工具不得自行停止 Clash TUN。

## 7. 无私人节点的工具回归

`test/fixtures/mixed-direct-sanitization.json` 使用 direct outbound，并故意包含额外 LAN inbound、controller、文件日志和 `set_system_proxy`。只有仓内 fixture runner 会显式传入 `-AllowDirectTestOutbound`；普通严格线路探针默认拒绝 direct。fixture 必须只启动目标 loopback，三种代理请求返回预期状态，且不得创建指定日志或启动额外监听。

`test_mixed_probe.ps1` 会启动仓内 loopback HTTP 204 origin，并用 HTTP absolute-form、显式 HTTP CONNECT tunnel 与 SOCKS5 请求验证两个 direct fixture，不依赖公网；结束后 origin 端口必须释放。非 loopback、TUN、系统时钟写入、top-level endpoint 和动态 outbound fixtures 必须在启动 core 前被拒绝。错误认证和主/辅助端口映射 contract 仍需补齐。

2026-07-20 使用本轮构建目录 core 的该组 fixture 为 7/7，额外 listener、系统代理、禁用日志与 origin 清理均通过。它只证明本地入口/工具收紧，不改变 AnyTLS + Trojan detour 和 Windows TUN/WFP 的发布阻断结论。
