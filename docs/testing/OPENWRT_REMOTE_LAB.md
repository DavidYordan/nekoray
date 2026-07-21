# OpenWrt 远程实验室

状态：现行测试手册
最后更新：2026-07-22

## 用途

本机 `D:\Program Files\nekoray` 的生产 TUN 必须保持工作，它可能使本项目的真实出站归因不清。此时可在 `192.168.1.7` 运行一个完全隔离的临时 sing-box 探针，判断问题属于配置/节点链路，还是 Windows 路由与生命周期。

测试机运行 OpenWrt 23.05.6，现有 RouteFluent core 与本项目本地 core 均为 `1.13.12-routefluent-anytls-client.7`。因此它适合验证相同 schema 和出站实现；OpenWrt 不是产品目标，成功结果不能替代 Windows 验收。

## 绝对边界

### 本机

- 不停止、不重启、不改写 `D:\Program Files\nekoray`。
- 不占用、不探测为本项目成果的 `2080`；本项目默认端口是 `12080`。
- 不修改生产系统代理、TUN、路由或 DNS。不能安全归因时直接使用远程实验室。

### OpenWrt

- 不停止、不重载、不修改现有 RouteFluent/sing-box 服务，也不修改 `/etc` 下现有配置。
- 不修改 `nftables`、防火墙、策略路由、路由表、DNS 服务或网络接口。
- 临时 Mixed 固定监听 `127.0.0.1:52080`；若端口已占用则中止，不自动换端口，不结束占用者。
- 每次只使用 `/tmp/nekoray-mixed-probe-<run-id>`；启动前记录现有服务 PID/命令行和监听，结束后复核它们未变。
- 只结束临时目录中 PID 文件指向、且 `/proc/<pid>/cmdline` 已核对为本次配置的进程。禁止 `killall`、`pkill` 和宽泛模式匹配。
- 清理只能删除本次精确临时目录。探针异常时先保留证据并确认进程身份，再处理；不得递归删除 `/tmp` 或通配目录。

## 能验证与不能验证的内容

| 能验证 | 不能替代 |
|---|---|
| sing-box 配置 schema 与 `check` | Windows Wintun/网卡生命周期 |
| loopback Mixed 的 HTTP、HTTPS CONNECT、SOCKS5h | Windows 系统代理注册表与用户会话 |
| Trojan、AnyTLS 及 detour 有/无对照 | WFP kill-switch 与 IPv4/IPv6 防泄漏 |
| DNS 解析、TCP/TLS/协议错误分类 | Windows GUI 退出、更新和重启 |
| 同版本 patched core 的实际出站闭环 | Windows 线路重启、PID/服务接管、接口所有权 |

`route.auto_detect_interface` 在此实验室中只控制 OpenWrt 的底层接口选择；它不负责把 Mixed 端口映射到主/辅助 profile。探针默认原样保留导出值，不再因为目标是 Linux 就强制改成 `true`。只有明确调查接口选择时才可使用 `--force-auto-detect-interface`，并必须把结果标为诊断变体。

## 连接资料

使用 RouteFluent 已有 helper 和凭据文件，不把密钥复制到本项目：

```powershell
$rfRoot = 'D:\complex\RouteFluent'
$rfPython = Join-Path $rfRoot '.venv\Scripts\python.exe'
$rfSsh = Join-Path $rfRoot 'scripts\remote_ssh.py'

& $rfPython $rfSsh device '/usr/bin/sing-box version'
```

helper 从 RouteFluent 的 `remote.env`/`scripts\remote.env` 读取连接资料。即使测试机不含私人数据，也不得把密钥、节点凭据或完整导出配置写入提交、共享日志或命令行回显。

项目探针直接复用同一组已忽略凭据和私钥，并固定校验 SSH host key、远端 core 版本与哈希。先执行本地收紧预演：

```powershell
& $rfPython .\tools\verify_mixed_openwrt.py <exported-config.json> `
  --dry-run --json
```

确认摘要中的入口、目标 outbound、detour 状态和变换项符合预期后，才执行真实探针：

```powershell
& $rfPython .\tools\verify_mixed_openwrt.py <exported-config.json> --json
```

AnyTLS 对照每次单独运行，不能把多个变量合并成一个结论：

```powershell
# 移除所选 AnyTLS outbound 的 detour
& $rfPython .\tools\verify_mixed_openwrt.py <exported-config.json> `
  --remove-detour --json

# 对照 patched core 的 AnyTLS client 实现
& $rfPython .\tools\verify_mixed_openwrt.py <exported-config.json> `
  --anytls-client native --json

# 仅接口诊断；默认测试禁止使用
& $rfPython .\tools\verify_mixed_openwrt.py <exported-config.json> `
  --force-auto-detect-interface --json
```

工具会拒绝源配置中的 TUN/TProxy、非预期入口/出站映射、平台专用绑定和漂移的远端身份；它还会取得互斥锁、记录前后基线并执行精确清理。任何安全检查或清理复核失败都按失败处理，不得手工跳过后继续批量运行。

## 标准探针流程

1. **本地导出与收紧**：从临时 appdata 导出目标 profile；在副本上执行 `nekobox_core check`。只保留 `127.0.0.1:52080` 的一个 Mixed，以及目标 outbound 沿 detour 可达的精确闭包；缺失/循环 detour、direct/block/selector/urltest 目标会失败关闭。拒绝 TUN、非空 top-level endpoints 和 `ntp.write_to_system=true`，移除无系统写入的 NTP、系统代理标记、其他 inbound、controller/service、源配置文件日志以及仅 Windows 可用的路径；日志改为仅写本次临时目录中的 `core.log`。探针会把临时 route 改成 Mixed → 显式目标 outbound，因此只证明目标链可连，不能证明原始主/辅助端口映射正确；DNS、detour 和 `auto_detect_interface` 默认保持，诊断覆盖每次只改变一个归因变量。
2. **只读预检**：记录远端 `/usr/bin/sing-box version`、现有 sing-box PID/命令行和监听；确认 `52080` 未占用、`/tmp` 可写。任一条件不满足即中止。
3. **唯一目录**：生成不可复用的 `<run-id>`，目标严格为 `/tmp/nekoray-mixed-probe-<run-id>`。上传配置后设为仅 root 可读，并记录本地/远端文件哈希。
4. **先 check 后启动**：用 `/usr/bin/sing-box check -c <temp-config>` 验证。失败时不启动。工具以 `/sbin/start-stop-daemon` 启动临时 core，把 PID 和 `core.log` 都限制在本次目录，并通过 `/proc/<pid>/cmdline`、配置路径和 `127.0.0.1:52080` 监听三项核对所有权。
5. **闭环请求**：分别验证 HTTP absolute-form、HTTPS CONNECT 和 SOCKS5h，记录精确期望状态、DNS/outbound 事件与错误类别。若测试 detour 或 AnyTLS 参数，每个变体使用新的 run-id，不原地改运行配置。
6. **精确清理**：只向已核对的临时 PID 发送终止信号，等待退出；确认 `52080` 释放后删除本次精确目录。再次核对原 RouteFluent 服务 PID/命令行与既有监听没有变化。

`tools/verify_mixed_openwrt.py` 自动执行上述安全边界，但执行者仍须保留它的 JSON 结果，并核对 `baseline_unchanged=true`、`remote_directory_cleaned=true`、三种协议的精确状态与 outbound 日志证据。不得因为远端现有代理可返回成功状态，就把它计为本项目临时 probe 的通过结果。

## 已验证基线

以下 2026-07-20 首轮 A/B 使用了旧版探针的“临时强制 Linux `auto_detect_interface=true`”变体。它仍能用于比较相同临时接口策略下的 AnyTLS/detour组合，但不能证明原始导出配置的接口策略。工具已修正；需要按默认 preserve重跑后，才可把新结果记为导出配置证据。

所有真实探针都固定在 `192.168.1.7` 的 `127.0.0.1:52080`。测试前现有 RouteFluent core PID 为 `24565`；每一轮结束后的 PID/命令行、配置哈希、manifest 哈希和监听集合均与测试前完全一致，且每轮远端临时目录均已删除。这证明本轮探针没有改动或重启既有服务；PID 只是一轮审计证据，后续仍必须动态取基线，不能硬编码。

| 临时配置变体 | 三协议结果 | core 日志/诊断 | 结论 |
|---|---|---|---|
| 保留主配置：`proxy` 为 AnyTLS `mihomo/1.19.28`，detour 为 `g-2` | HTTP 502；HTTPS CONNECT 超时；SOCKS5h 空响应 | Mixed 事件 8、目标 outbound 事件 7；AnyTLS `failed to create session: EOF` | Mixed 已接收并送入目标链，但当前组合不能闭环 |
| 移除 detour，保留 AnyTLS `mihomo/1.19.28` | HTTP/CONNECT/SOCKS5h 均为 204 | Mixed 事件 8、目标 outbound 事件 3 | 该 AnyTLS client/线路在同一 core 上可闭环 |
| 移除 detour，并改用原生 AnyTLS client | 整轮失败 | 服务端返回 internal error | 不能把“移除 client 标记”作为修复 |
| 独立 profile 2 Trojan（完整对象与 `g-2` 相同） | HTTP/CONNECT/SOCKS5h 均为 204 | 三种入口协议均命中目标 outbound | detour 对象本身可用 |

这组对照已排除“Mixed 根本不能连接”、AnyTLS mihomo client 单独不可用、以及 `g-2` Trojan 对象单独不可用。现有证据把故障收敛到 “AnyTLS mihomo client 经 `g-2` detour” 这一组合或其生成/运行语义；它尚不能单独证明具体是哪一层实现缺陷。

## 结果解释

- **OpenWrt 成功、Windows 失败**：配置和节点链在相同 core 上可用；优先检查 Windows TUN 回环、默认路由、接口选择、WFP 和 GUI/core 生命周期。
- **两边同一收紧配置都失败**：优先检查配置生成、DNS、detour、节点兼容或上游网络；仍需确认两边改变的变量一致。
- **只有某个协议失败**：Mixed 已监听不等于出站成功；按 HTTP/CONNECT/SOCKS5h 和 DNS/TCP/TLS/outbound 事件分层定位。
- **只有组合失败、两个组成出站分别成功**：优先检查 detour 连接语义、配置生成和协议组合兼容；不能继续把故障归到 Mixed，也不能用任一单独成功宣称主配置已通过。
- **AnyTLS 原生 client 返回服务端 internal error，而 mihomo client 成功**：保留当前服务端所需的 client 兼容语义，不能通过删除 client 字段掩盖 detour 问题。

## 何时停止并申请维护窗口

OpenWrt 无法回答 Windows Wintun、系统代理、WFP、GUI 退出/重启和线路重启问题。应先使用独立 Windows 测试环境；只有问题必须依赖本机 Windows 的真实接口/驱动，且生产 TUN 的独占资源或默认路由使测试无法成立时，才停止继续推断并请用户安排维护窗口。

维护窗口申请必须列出：要验证的 Windows 专有断言、为何 OpenWrt/独立 Windows 不足、需要暂停生产网络的最短阶段、无直连泄漏的保护措施、精确恢复与复核步骤。任何 agent 或自动化都不得自行开启该窗口。
