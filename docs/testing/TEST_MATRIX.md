# 测试矩阵

状态：现行
最后更新：2026-07-21

## 证据要求

每次发布候选至少记录：提交号、GUI/core 文件哈希、Windows 版本、命令、预期结果、实际结果和脱敏报告路径。日志、导出配置和报告可能含节点名、服务器地址、路由、进程及本地路径，不得提交真实凭据。

Windows 是唯一产品平台。OpenWrt 是相同 core 的诊断环境，不是产品兼容目标，也不能代替 Windows 发布验收。

历史一次性 Mixed/AnyTLS 调查和带日期的实验结果不再复制到现行矩阵；见 [2026-07-20 接管基线](../archive/audits/2026-07-20-takeover-baseline.md)。历史中的单次成功不能升级为当前通过结论。

## 2026-07-20 无侵入回归

| 项目 | 结果 | 证明范围 |
|---|---:|---|
| 当前源码 Windows GUI 增量构建 | 成功 | 只证明本构建树可编译 |
| 两个 Go 模块普通测试 | 通过 | 包含现有 nil-config/system-fallback/FullTest 窄回归 |
| 两个 Go 模块 `go test -race ./...` | 通过 | 使用仓库 MinGW 与 `CGO_ENABLED=1` 实际执行；仍未构造完整 GUI/RPC lifecycle 竞争 |
| `test_final_config_guards.ps1` | 10/10 | 隔离导出路径的部分最终配置 guard |
| `test_config_preservation.ps1` | 7/7 | 损坏主/路由、错误辅助映射、未知 profile 与悬空 group 引用均保持原件不变并生成可验证隔离证据 |
| `test_verify_mixed_openwrt.py` | 19/19 | 远端 helper 收紧与命令安全单测；未执行真实远端协议重测 |
| `test_mixed_probe.ps1` | 7/7 | loopback Mixed、拒绝项、额外 listener/系统代理/日志/origin 清理 |
| `test_runtime_connectivity.ps1` | 正例通过、反例正确拒绝 | expected 204 时 HTTP/SOCKS5h 均 204；expected 200 时报告 2 项 mismatch；系统代理、端口与 origin 清理通过 |
| CTest | 1/1 | `config_recovery_test` 覆盖内容寻址备份、幂等、篡改/越界拒绝、隔离元数据原因合并和外部修改竞争；不代表 ConfigBuilder/GUI 已有 C++ golden |
| Windows quality CI | 已建立，待远端首次运行 | 仓库卫生、固定子模块、受控 core 源构建、Go 普通测试和 verifier 安全契约；不覆盖 GUI/TUN/WFP |

本轮测试使用重建后的 `build-package-windows64/nekobox.exe` 和 `nekobox_core.exe`；`deployment/windows64/` 仍是旧产物。以上均不是 Windows TUN/WFP、系统代理 broker、GUI 退出或线路切换验收。

## 三级验证矩阵

| 级别 | 范围 | 必测项目 | 当前结论 | 发布要求 |
|---|---|---|---|---|
| L1 本地无侵入 | 配置/schema | 每个导出配置执行 `nekobox_core check`；空配置、迁移、损坏配置 | 已验证损坏主/路由配置及错误类型、非字符串、重复辅助映射原件不被覆盖；其它迁移矩阵不完整 | 必须自动化通过 |
| L1 本地无侵入 | Mixed contract | HTTP absolute-form、HTTPS CONNECT、SOCKS5h、认证正反例、端口占用、非 loopback/TUN 拒绝 | 正向及安全收紧有证据，反例不完整 | 必须全部通过 |
| L1 本地无侵入 | 端口映射/OS 副作用 | 主 `12080` 命中当前主 profile；每个辅助端口命中其绑定 profile；顶层 custom 不得改变完整受管 listener/outbound 快照；未授权/非精确 TUN、`set_system_proxy=true` 及已知系统 endpoint/时钟副作用必须拒绝 | 生成逻辑已审计；导出 fixture 仅覆盖部分 OS guard，Mixed/TUN C++ golden 仍缺 | 必须通过 |
| L1 本地无侵入 | 工具安全 | 不改系统代理/TUN/路由/DNS；拒绝 TUN、系统 NTP 写入和非空 endpoints；只保留目标 outbound detour 闭包并只结束精确 PID；不触碰 `D:\Program Files\nekoray` 和 `2080` | 本地/远端收紧器已有 fixture；OpenWrt Python 单测 19/19 | 必须保持 |
| L2 OpenWrt 探针 | core/工具安全 | 相同 `1.13.12-routefluent-anytls-client.7` core 的 schema、loopback Mixed、监听 PID 与远端基线保护 | 已真实执行；每轮既有 PID/命令行、配置/manifest 哈希和监听均不变，临时目录已清理 | 必须保持 |
| L2 OpenWrt 探针 | 远端链路 | Trojan、AnyTLS、DNS、detour 有/无的对照；HTTP/CONNECT/SOCKS5h | AnyTLS mihomo 无 detour与独立 profile 2 Trojan 均三协议 204；该 Trojan 对象经比对与 `g-2` 完全相同；主 AnyTLS + `g-2` detour 失败；原生 AnyTLS client 被服务端拒绝 | 主组合阻断发布 |
| L3 Windows 集成 | 生命周期 | GUI 退出/重启、线路重启、core 崩溃与回滚 | 未验收 | 阻断发布 |
| L3 Windows 集成 | 网络控制 | 系统代理、Wintun、persistent WFP kill-switch、IPv4/IPv6、DNS、PID/资源所有权 | 内部 TUN 活动配置已强制 strict+IPv4/IPv6，但 WFP 仍随 worker 消失；系统代理 UI 暂禁，精准 broker 未实现 | 阻断发布 |
| L3 Windows 集成 | GUI/安装更新 | clean build、干净用户目录、安装/更新失败与回滚 | 增量构建有证据，完整矩阵未完成 | 阻断稳定版 |
| L1/L3 | C++/Go/脚本自动测试 | CTest、自有 Go 模块与隔离导出 fixture | CTest 现有恢复基础设施 1 项；两个 Go 模块的普通与 race 测试已有通过证据，含 nil-config/system-fallback/FullTest 窄回归；PowerShell/Python fixture 有独立证据，但 ConfigBuilder/import/runtime 仍缺 C++ golden | 阻断稳定版 |

## 逻辑线路与接口选择的断言

- 默认 Mixed 端口固定为 `12080`；`2080` 是外部生产实例，不是兼容或迁移目标。
- 主/辅助 Mixed 入口必须强绑定各自逻辑 profile/outbound。标准路由规则不得将其改送 `direct`、`block` 或另一条线路；最终 custom 合并若破坏绑定必须在启动前失败关闭。
- 断言对象不仅是 tag/port/detour：合并前捕获的完整受管 listener 和全部可达 outbound 必须在合并后逐项一致，且精确 terminal binding 必须先于可能改投/提前 resolve 的规则。这个断言不自动覆盖未受管的所有路由/DNS 语义。
- `auto_detect_interface` 仅是底层 OS 接口选择，不能改变入口到逻辑线路的绑定，也不能作为“Mixed 自动选线路”的验收依据。
- 本机生产 TUN 存在时，本地远端成功只能证明请求完成，不能单独证明物理出站归属。此时按 [OpenWrt 远程实验室](OPENWRT_REMOTE_LAB.md) 做 L2 对照。

当前 L2 详细结果以 [已验证基线](OPENWRT_REMOTE_LAB.md#已验证基线) 为唯一现行记录。它证明 Mixed 能把请求送到指定逻辑 outbound，也把当前故障收敛到 AnyTLS mihomo client 与 `g-2` detour 的组合；不证明 Windows Wintun、WFP、系统代理、GUI 或线路重启已通过。

## 发布前最小矩阵

1. 在干净的 Windows 用户目录启动，不复用唯一真实配置，不接触生产 Nekoray。
2. 覆盖空配置、旧配置迁移、损坏配置和恢复流程。
3. 完成 Mixed 三种协议、认证正反例、端口占用、非法端口与主/辅端口映射。
4. 完成 Trojan、AnyTLS 及有/无 front proxy；L2 只能补充远端归因，最终仍需 L3 Windows 证据。
5. 验证只有用户手动操作才能启停系统代理/TUN；GUI 退出、重启和线路切换不改变它们。
6. 分别核对 TUN requested、worker-observed 和 Windows 实际状态；core 崩溃后当前只恢复空控制 core的行为必须被识别为未通过，而不是把 requested 误判为 TUN 已恢复。
7. 在保护持续生效时完成线路重启、失败回滚和故障注入，证明不会 direct 泄漏。
8. 覆盖 worker 无 instance、worker kill、service/BFE restart，确认 TCP、UDP、HTTP helper 和物理 IPv4/IPv6/DNS 均无系统 fallback。
9. 覆盖空订阅、HTML 错页、超时、解析失败和正常更新；失败时旧组原样保留。
10. 覆盖更新失败、校验失败和回滚；修复更新器前不得进行真实更新测试。

## 工具入口

- `tools/export_profile_core_config.ps1`：导出单个 profile 的实际 core 配置并可执行 `check`。
- `tools/verify_mixed_inbound.ps1`：只接受主 `mixed-in -> proxy` 连通性诊断；拒绝 TUN/系统 NTP 写入/endpoints、裁剪到 `proxy` 的精确 detour 闭包后启动临时配置，验证监听 PID 和三种代理请求；不是辅助映射 contract 或 Windows 集成验收器。
- `tools/verify_mixed_openwrt.py`：在固定 `192.168.1.7`、`127.0.0.1:52080` 和唯一临时目录中执行相同的严格收紧与 L2 探针；必须先 `--dry-run`，真实结果须满足远端基线未变和清理完成。
- `test/fixtures/mixed-direct-sanitization.json`：验证诊断脚本不会启动额外 LAN inbound/controller、修改系统代理或写配置指定日志。
- `test/test_mixed_probe.ps1`：用 loopback HTTP 204 origin 运行 direct、dummy auth 与安全拒绝 fixtures，并核对端口、系统代理、文件副作用和 origin 清理；不依赖公共站。2026-07-20 为 7/7。
- `test/test_verify_mixed_openwrt.py`：覆盖远端收紧器的接口字段、NTP/endpoints、精确 detour 闭包、敏感摘要与命令安全；当前 19/19。
- `test/test_runtime_connectivity.ps1`：在临时 package 和 loopback HTTP 204 origin 中验证运行快照脚本的 PID 归属、精确 HTTP 状态、错误期望拒绝及清理；2026-07-20 的 204 正例通过，错误期望 200 正确失败。
- `test/test_config_preservation.ps1`：在隔离 appdata 中启动配置导出路径，验证已存在但损坏的主/路由配置、重复/非字符串/错误 JSON 类型的辅助映射、未知 profile 类型与悬空 group 引用保持 SHA-256 不变，并验证 snapshot/metadata；当前为 7/7。
- `test/test_final_config_guards.ps1`：在隔离临时 appdata 中验证安全 `internal-full` 文件导出、部分 TUN/系统代理/system endpoint/NTP 副作用拒绝，以及标准 SOCKS profile 的测试态 custom、Managed Mixed listener、detour 禁改和 custom route 空字段回归；2026-07-20 为 10/10。不启动线路，不代表完整 C++ golden 覆盖。
- `tools/verify_runtime_connectivity.ps1`：采集整套运行时快照；不是自动验收器。
- `tools/verify_fail_closed_restart.ps1`：采集 fail-closed 相关状态；使用限制见 [fail-closed 验证](FAIL_CLOSED.md)。

这些工具调用的 `nekobox_core check/run` 是[显式高级 CLI](../reference/CLI.md#core-高级-cli)，会直接读取经过导出或收紧的临时配置；它们不走普通 GUI，也不能用于证明 Go 层已经独立实施产品策略。
