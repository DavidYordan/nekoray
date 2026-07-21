# 接管状态

状态：Alpha / 不可发布
基线：NekoRay 4.0.1 `adef6cd` → `96f1166`，现行接管分支 `agent/takeover-remediation`
最后更新：2026-07-21

## 结论先行

上一阶段已经明显偏离需求：它不仅增加 AnyTLS、并发线路和 Clash server-domain DoH，还删除了多项 NekoRay 能力、加入 MultiMapper 与复杂 resolver 工具，并用“禁止 TUN 下重载”代替无泄漏切换。因此当前分支不能按既有方向继续堆功能，必须先恢复最小分支边界。

当前“Mixed 根本无法连接”的判断也不准确。隔离测试证明 Mixed 能接收 HTTP、CONNECT、SOCKS5h 并进入指定 outbound；现有主配置失败集中在 **AnyTLS(Mihomo client) 经 Trojan detour** 的组合链。

## 审计结果

`adef6cd..96f1166` 有 36 个提交、约 117 个文件变化、约 +8,497/-3,236 行。主要偏离为：

| 等级 | 发现 | 处理方向 |
|---|---|---|
| P0 | external-core、Naive、custom external、TUIC/Hysteria2 外核被整体删除 | 选择性恢复；仅 Xray 保持删除 |
| P0 | 未知/现已不识别 profile 曾被 loader 删除 | 已停止删除与 ID 复用，并生成可验证 quarantine 证据；仍缺可操作恢复 UI 后再恢复模型 |
| P0 | 订阅刷新曾在验证前改 order/删 profile | 已改为 parse/stage/validate 后提交；继续补多文件事务与故障注入 |
| P0 | TUN 下重载被 UI 阻止；整核 Stop/Start 无独立 kill-switch | 持久 runtime + WFP + generation 事务 |
| P0 | final custom 曾可覆盖 Mixed 端口绑定；空辅助 chain 曾可 fail-open | 最终 validator 已落地；补完整负向自动回归 |
| P0 | DoH 扩展曾含普通 nameserver 猜测、本机 fallback、无 DoH也套自定义 group | 已收敛为精确 proxy-server-nameserver + strict；bootstrap 仍待实现 |
| P1 | VMess/v2rayN、SOCKS userinfo、SS v2ray-plugin 等格式兼容误删 | 恢复，不等同于恢复 Xray core |
| P1 | GeoSite 自动完成变成空 UI；测试路径曾出现直连/无界风险 | URL Test 已恢复为有界配置；TCP Ping 已禁用；GeoSite 仍待恢复 |
| 越界 | MultiMapper、复杂批量解析/改 IP、通用健康探测平台 | 退出产品主线，材料归档 |

完整逐提交证据见 [范围偏离审计](archive/audits/2026-07-20-scope-deviation-audit.md)。

## Mixed 与 AnyTLS 的真实证据

OpenWrt `192.168.1.7` 使用同版本 `sing-box 1.13.12-routefluent-anytls-client.7` 的临时 loopback 探针，未修改现有 RouteFluent runtime：

| 配置 | HTTP | CONNECT | SOCKS5h | 结论 |
|---|---:|---:|---:|---|
| AnyTLS `mihomo/1.19.28` + `detour=g-2` | 失败 | 失败 | 失败 | AnyTLS create-session EOF |
| 同一 AnyTLS，仅移除 detour | 204 | 204 | 204 | AnyTLS/Mihomo 单跳可用 |
| 移除 detour并改 native | 失败 | 失败 | 失败 | 服务端 internal error |
| 独立 Trojan profile 2（与 `g-2` 对象相同） | 204 | 204 | 204 | Trojan 单跳可用 |

因此 Mixed 监听、主端口映射、Mihomo AnyTLS 单跳和 Trojan 对象分别成立；组合 detour 尚未闭环。旧探针会在临时 OpenWrt 副本强制 `auto_detect_interface=true`，所以这些结果只能用于协议组合归因，不能证明导出配置的接口策略；工具现已改为默认 preserve，后续需按新默认重跑。

## 已完成的接管止损

- 明确产品边界：保留 NekoRay，只有 Xray 明确删除；新增仅三项。
- 产品生成器恢复上游 `auto_detect_interface=dataStore->spmode_vpn`；测试机适配不得进入默认。
- OpenWrt 探针默认保留该字段，仅显式诊断参数可强制；收紧器会拒绝系统 NTP 写入/非空 endpoint，并把 outbound 缩到目标线路的精确 detour 闭包；当前 Python 工具单测 19/19 通过。
- 主 Mixed 默认 `12080`；Clash API 默认保持关闭（配置值 `-9090`，启用时端口 `9090`）；`2080` 与生产安装完全隔离。
- 主/辅助 Mixed 标准路径显式绑定各自 chain；辅助 chain 失败、profile失效、端口重复现在整体构建失败，不再留下孤儿入口。
- 主/辅助 Mixed 都不再在精确线路绑定前借全局/默认 DNS 执行 `resolve`；使用 provider server-domain DoH 的 outbound 会绑定精确 strict resolver。无 provider DoH 的普通节点仍走 NekoRay/sing-box 原有解析路径。
- 没有 provider DoH 的普通节点不再强制使用自定义 `local_only` resolver group。
- Clash 导入只读取 `proxy-server-nameserver`/`proxy_server_nameserver`；字段存在但没有有效 HTTPS DoH 时整次导入失败且旧数据不变。
- 生成配置已移除 local fallback/probe；DoH endpoint 仍是域名且没有可审计 bootstrap 时明确构建失败，不再偷用 `local-system`。
- 顶层 custom 合并前会捕获每个受管 Mixed 的完整生成 listener 和沿 detour 可达的全部 outbound 对象；合并后要求这些对象逐项相同、各 tag/port 唯一且精确无条件 route 绑定仍在所有可能改投/提前 resolve 规则之前。profile 级 `custom_outbound` 可在快照前修改普通字段，但不得新增或改变 detour。provider resolver 的 outbound → strict group → 精确 DoH server 也按生成对象锁定，并拒绝 RouteFluent fallback/local-only 字段。
- 辅助端口 map 不再因 stop/restart/crash/exit 或 UI 刷新被清空；字段类型错误、非字符串、损坏或重复项会使既有主配置原件保持不变并中止启动。显式启停/删除映射只有在原子保存成功后才继续 reload，失败会回滚内存。
- 配置文件改用禁止 direct-write fallback 的 `QSaveFile` 原子替换；覆盖前建立可验证备份，损坏/未知 profile 原件保留并生成 quarantine，且 ID 不复用，危险 reorder 已禁止。单 profile/空 group 删除和 profile 跨组移动已接入带 before/after 清单、进程/跨进程串行锁和失败逆序回滚的事务层；未完成状态会阻断当前保存和下次启动。外部修改、引用关系或证据失败时拒绝删除，非空 group 的旧半删除路径仍整体禁用。
- route 名现限制为安全 Windows 单文件名，非法 `active_routing` 保留主配置原件并中止加载；非活动 route 使用事务删除，活动 route 必须先显式切换，不再直接删除后尝试补救。
- 订阅刷新恢复为 parse/stage-before-mutate；解析失败、空响应、坏结构与零支持节点不写入。清理/回滚删除失败会保留对象并明确记录，但成功候选仍未形成跨文件崩溃一致事务。
- Go helper 在无有效 sing-box instance 时不再回落系统 TCP/UDP/HTTP 网络。
- Windows legacy 外置 TUN 因无精确 PID/句柄且曾按映像名批量 `taskkill`，现已安全禁用；配置数据保留，默认内部 TUN不受此项删除。
- Windows 内部 TUN 生成配置现强制 `strict_route=true` 并同时覆盖 IPv4/IPv6；这只收紧活动期，仍不能覆盖 worker/GUI 消失窗口。
- 最终 OS-mode validator 会拒绝未由显式产品 TUN 开关生成的 TUN，并要求受管 TUN 完整对象与生成值精确一致；在受管 TUN 活动配置中还拒绝 `route.default_interface` 以及 outbound/endpoint/DNS 的 bind-address/interface 覆盖。任意模式下的 inbound `set_system_proxy=true`、系统 WireGuard/Tailscale endpoint 与 NTP 写系统时钟均拒绝。它是已知 OS 副作用与产品 TUN 对象的窄 guard，不代表任意 `route.rules` 都已形式化证明安全。
- 无令牌的 `-flag_restart_tun_on` 自动提权连续流程已删除；非管理员用户须自行以管理员身份启动后再次手动启用 TUN。
- legacy WinINet 系统代理接口不能证明写入成功、所有权或完整恢复原 PAC/proxy/bypass，产品内 Windows 系统代理切换现已临时禁用，等待按 SID 的 compare-and-restore broker。
- MultiMapper 专用导出和复杂批量 resolver/change-IP 平台已从产品 UI/构建/代码移出，历史材料保留在 archive。上游简单 **Resolve domain** 因直接走 Windows 系统 resolver 并永久覆盖节点域名，现也改为无副作用禁用说明；未来只能经对应 provider resolver 且保留原域名。
- 打包脚本不再关闭/强杀运行实例；发现运行即失败。生产安装不再是默认构建参考目录。
- 启动/普通重启不再根据 remember 状态或 CLI 连续参数自动启用系统代理/TUN；core 崩溃时只重启空控制 core，不自动恢复 profile/TUN。
- Windows GUI 已忽略 CRT `SIGTERM`/`SIGINT`，避免控制台信号直接绕过 UI 退出 guard；这不处理 `TerminateProcess`/任务管理器强制结束、崩溃、关机或 worker 自退，持久 Runtime/WFP 仍是 P0。
- URL/Full Test 只接受精确有界的临时生成配置，`internal-full` 与顶层 `custom_config` 变更会被测试路径拒绝；产品 TUN requested/worker active 任一成立时拒绝新测试，测试运行中也拒绝启 TUN。Full Test 中原先调用系统 DNS 的“入口 IP”查询已禁用，并补上空配置/非法 URL 拒绝、父 RPC context 取消、超时 goroutine 防阻塞和 64 KiB 响应上限；TCP Ping 在 GUI 与 core 两层都明确禁用，因为它使用系统直连 socket。
- 默认文件导出和 `for_share` 别名都使用无产品 TUN/辅助运行态的审计导出；`for_test` 使用独立有界测试配置。所有模式都执行已知 OS 副作用校验，但导出文件仍含凭据且不能视为可任意启动的沙箱。
- 普通 GUI 通过 localhost、每次启动随机令牌的 gRPC 控制 core。独立 `nekobox_core run/check` 是构建与隔离测试显式使用的高级入口，可直接读取 sing-box 配置；Go 层尚未重复 C++ 的产品策略校验，属于需要补齐的纵深防御边界，而不是普通 GUI 可随意绕过 guard 的证据。
- 本轮 GUI/core 只重建到 `build-package-windows64/` 并用于接管验证；`deployment/windows64/` 仍是 2026-07-18 的旧产物，未完成正式全量打包，不可交付。

## 2026-07-21 最终无侵入回归快照

- 当前源码的 Windows GUI 增量构建成功；两个 Go 模块普通测试通过，本轮较早也用仓库 MinGW、`CGO_ENABLED=1` 通过两个模块的 race 测试，随后已重建 `build-package-windows64/nekobox_core.exe`。
- 本轮构建目录快照：`nekobox.exe` SHA-256 `E9D62A125390D7C28552727FBC9494FF5A19FAACDFD0F7595264AD666E868E12`；`nekobox_core.exe` SHA-256 `D2D532E72CEE65791D5A098D688FB6C9A9F0133C2C79B847070627E595656E92`。这只是接管审计证据，不是 release manifest。
- `test_final_config_guards.ps1` 10/10，`test_config_preservation.ps1` 9/9，`config_recovery_test` 1/1，OpenWrt helper Python 单测 19/19。
- 本地 Mixed fixture 7/7；额外 listener、系统代理、禁用日志和 loopback origin 清理均保持预期。
- runtime connectivity 的 expected 204 场景通过，HTTP 与 SOCKS5h 均为 204；expected 200 场景按预期报告 2 项 mismatch 并返回失败。系统代理、fixture 端口和 origin 清理均通过。
- 本轮已执行 `ctest --test-dir build-package-windows64 --output-on-failure`，`config_recovery_test` 为 1/1；它覆盖事务基础的提交、模拟回滚、锁和 pending 阻断，不覆盖订阅/非空 group 或 ConfigBuilder golden。远端 Windows quality CI 已通过。以上证据均不覆盖 Windows TUN/WFP/退出/切线，也不改变 Alpha/不可发布判断。

## 仍然阻断发布

1. external-core/Naive 与误删格式兼容尚未恢复；未知数据已保留并生成 quarantine，覆盖和显式删除已有恢复证据，但恢复 UI 仍缺，非空 group 删除因此暂时拒绝。
2. 单文件原子保存与删除事务基础已落地，订阅也已先 stage 并报告删除失败；订阅成功候选仍未接入事务层，也缺真实进程中断/磁盘故障和人工恢复验收。
3. 最终 Mixed、strict resolver 与 TUN/系统代理副作用不变量校验已落地，并新增 `test/test_final_config_guards.ps1` 覆盖部分导出拒绝分支；完整 C++ 配置生成 golden/负向回归仍未完成，不能只凭脚本存在判定通过。
4. AnyTLS + Trojan detour 仍失败。
5. 当前 Go wrapper 没有原地热重载；内部 TUN 与单个 Box 同生共死，WFP kill-switch/持久 Runtime Service 尚不存在。
6. Windows 下主+多辅助真实不同出口、TUN 切线、worker/GUI 崩溃窗口与 IPv4/IPv6/DNS 防泄漏尚未验收；当前 guard 仍违反“开 TUN 时可切线”。
7. 精准系统代理 broker 尚未实现，产品内切换暂禁；旧 WinINet helper 不得重新接回 UI。
8. Go core 仍缺产品策略的第二层校验，以及统一保护 `Start`/`Stop`/stats/全局 instance 的 lifecycle mutex/generation；localhost 随机令牌只保护 RPC 调用，不解决配置授权和并发状态机。
9. Windows-only CI 已建立并通过，但干净 Qt/MinGW/C++ 依赖工具链和真实交付二进制 manifest 尚未完成；libneko 已锁定为仓内子模块，当前 `deployment/windows64/` 仍是旧产物。

本分支不得部署到 `D:\Program Files\nekoray`。后续顺序见 [推进路线](ROADMAP.md)。
