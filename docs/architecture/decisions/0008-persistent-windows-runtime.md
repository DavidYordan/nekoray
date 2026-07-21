# ADR 0008：持久 Windows Runtime、稳定入口与无直连切换

状态：Proposed
日期：2026-07-20
最后复核：2026-07-20

## 背景

[ADR 0004](0004-runtime-safety-policy.md) 已冻结：只有精准的用户手动操作可以启停系统代理或 TUN；GUI 退出、重启和线路切换不得改变 OS 模式；任何故障都不得退回物理直连。

源码审计确认当前实现不能满足该契约：

- Go gRPC 只有 `Start`/`Stop`，已有 instance 时拒绝再次启动；没有 `Prepare`、`Commit`、`Rollback` 或原地 `Reload` RPC。
- `Stop` 关闭整个 sing-box Box，Mixed、DNS、outbound、Wintun、路由和 sing-tun 动态 WFP session 同时消失。
- 当前切线是先停旧 Box 再启动新 Box；候选失败时没有运行时事务回滚。
- WFP session 使用 dynamic 对象，随 worker/session 消失；它不能覆盖 worker 崩溃或切换空窗。
- 标准内部 TUN 已强制 `strict_route=true` 并同时生成 IPv4/IPv6 地址；旧持久字段不再控制该路径。但这是 worker 活动期收紧，不能覆盖 Stop、崩溃或 GUI 退出空窗。
- UI guard 要求 TUN 开启时不要切线/退出，只能回避正常路径。Windows GUI 已忽略 CRT `SIGTERM`/`SIGINT`，关闭了控制台信号绕过 guard 的窄入口；它仍无法约束强制进程终止、崩溃、系统关机或 worker 自退。
- UI 现将 `spmode_vpn` 的 requested state 与 `running_internal_tun`/外置 PID 的 worker-observed state 分开显示；该观测仍不是 Windows OS 事实。core 崩溃后只重启空控制 core，不自动恢复 profile/TUN，因此既不会隐式启用，也不会维持数据面。
- Go helper 曾在无 instance 时回落系统 TCP/HTTP，UDP 始终使用系统网络；接管工作树已改为明确失败，但这不等于建立了数据面保护。
- 普通启动/重启的自动模式恢复与无令牌 `-flag_restart_tun_on` 连续流程均已移除；非管理员用户必须自行以管理员身份启动后再次手动启用。
- legacy WinINet 系统代理实现缺 owner、完整快照、compare-and-restore 与写后回读，Windows UI 已暂时拒绝切换，等待本 ADR 的 user-session broker。
- 过渡最终配置校验已拒绝 sing-box inbound `set_system_proxy=true` 和未授权 TUN，要求受管 TUN 完整对象逐项等于生成值、保留接口自动检测且无 default/bind-interface 覆盖，并拒绝系统 endpoint/时钟副作用；`internal-full` 与产品 TUN、辅助并发和测试路径隔离。这能阻断已知旁路，但不是任意路由的形式化证明，也不能在 worker 消失后维持数据面。
- Windows legacy 外置 TUN 使用占位 PID，停止时曾按映像名清理所有其它 core；接管工作树已拒绝启停该路径，避免误伤外部生产实例。
- 普通 GUI 的 core RPC 仅监听 localhost，并用每次启动随机令牌鉴权；`nekobox_core run/check` 是另一个由用户显式调用、测试工具依赖的高级入口。Go 层尚未重复 C++ ConfigBuilder 的产品策略校验，未来 Runtime 必须统一配置授权和执行入口，但不能把这一缺口误写成普通 GUI 的任意旁路。

因此不能靠删除 guard、增加重试或把 `auto_detect_interface` 强制为 `true` 解决。后者只涉及底层接口/防环路，不负责 Mixed 入口到逻辑线路的映射，也不得承载本机双 NekoRay 的测试特例。

## 提议架构

```text
Qt GUI（控制面，可退出/重启）
  -> 带显式 ACL 的本地 named pipe
Windows Runtime Service（唯一网络运行时所有者）
  ├─ desired / observed / owner / health / generation journal
  ├─ persistent WFP fail-closed 边界
  ├─ 持久 Wintun/TUN anchor
  ├─ 稳定 Mixed anchor：主 12080 + 各辅助端口
  ├─ A/B worker generations：代理、AnyTLS、每线 DoH
  └─ user-session system-proxy broker（只接受手动命令）
```

Runtime Service 的职责：

- GUI 退出只断开控制连接，不停止已启用模式依赖的入口、TUN 或保护状态。
- 以句柄、PID、创建时间、规范路径、config hash 和 generation 管理 worker，禁止按映像名批量结束进程。
- 所有 GUI、gRPC、CLI 导入的候选配置在进入受管 runtime 前执行同一套产品策略校验；CLI `check` 的 schema/pre-start 成功不构成授权。
- 系统代理属于用户会话；仅 `EnableSystemProxy`/`DisableSystemProxy` 显式命令可由对应 SID 的 broker 修改，并执行 compare-and-restore。
- TUN 仅由 `EnableTun`/`DisableTun` 显式命令改变。Windows 重启后不得自动启用 TUN；服务应先维持/恢复保护边界、展示状态并等待用户操作。
- 只有真实拥有 TUN/路由的 runtime 根据 underlay LUID/路由快照决定接口防环路策略；不得由全局 `spmode_vpn` 或测试机例外推导。

## 稳定 anchor 与线路 generation

每个 Mixed 端口是固定逻辑线路，不是自动选择器：

- `12080` 只进入主线路。
- 每个辅助端口只进入其绑定辅助线路。
- 辅助故障只能使该入口失败；不得转到主线、direct、其它辅助线路或本机 DNS。

候选切换按事务执行：

1. 构建并完整验证 candidate 配置。
2. 在影子端口启动 generation B，验证协议、DoH、握手和目标线路。
3. 在 WFP transaction 中先加入新 endpoint 的精确 underlay 例外并确认保护已 armed。
4. 原子切换稳定 anchor 对应的逻辑 selector；旧 generation A 进入有界 drain。
5. B 异常则切回 A；A/B 都不可用时进入可观察的 `DEGRADED_BLOCKED`，不改变系统代理/TUN 模式。
6. drain 完成后移除旧 endpoint 例外并结束 A。

第一阶段可接受连接中断：持久 anchor/WFP 保持，切换窗口先阻断线路，再停 A、启动 B；B 失败则恢复 A，恢复失败继续阻断。第二阶段再实现 A/B 并存和低中断 selector 切换。两个完整 Box 不能同时直接占用同一 `12080` 和同一 TUN，因此“整 Box 双开后交换端口”不是可行的第一阶段设计。

## WFP 与权限边界

- WFP provider/filter 必须独立于 worker，使用可跨 worker 崩溃维持的 persistent 对象；规则更新使用 WFP transaction。
- 默认覆盖物理接口 IPv4、IPv6 和 DNS，只放行 loopback、项目 TUN、当前代理/DoH bootstrap 的精确 underlay，以及明确批准的 DHCP/NDP/LAN 例外。
- 服务异常恢复后保持 `BLOCKED_NEEDS_USER`，不得通过自动停 TUN 或恢复 direct 自愈。
- 安装/更新服务时一次性提权；GUI 保持普通用户。worker 使用受限 token 与精确 Job Object。
- 恢复工具只能处理本项目 provider GUID 和资源，不得触碰 `D:\Program Files\nekoray`、`2080` 或其生产 TUN。

## 测试边界

OpenWrt 只适合验证 AnyTLS、DoH、端口到线路映射和无跨线路 fallback。Wintun、WFP、SCM、Windows DNS/IPv6、GUI/worker/service 崩溃必须在隔离 Windows 10/11 环境验证。

最低故障矩阵包括 candidate 无效、端口占用、DoH/AnyTLS 失败、worker kill、GUI exit/crash/restart、service crash、BFE restart、NIC 切换与休眠恢复；必须在物理接口和 Wintun 抓包中同时断言没有未授权 IPv4、IPv6 或 DNS 包。

## 仍待确认

1. 线路切换是否接受短暂全阻断和既有 TCP/UDP 连接重置，还是第一版就必须保持连接。
2. kill-switch 保护全机还是本项目/指定进程；DHCP、NDP、LAN、RDP、打印机等例外范围。
3. DoH endpoint 域名的可审计 bootstrap 来源。

在威胁模型、实现和 Windows 故障注入完成前，本 ADR 保持 Proposed。无论这些体验细节如何选择，都不得削弱 ADR 0004 的手动模式控制、固定线路映射和绝不直连约束。

## 迁移顺序

1. 保留当前 guard 作为“尚不支持”的临时发布阻断，不把它当解决方案。
2. 建立单线程 RuntimeStateMachine 和精确资源所有权。
3. 引入 Runtime Service、认证控制通道与稳定 Mixed/TUN anchor。
4. 实现 persistent WFP、故障恢复和显式模式命令。
5. 实现受保护的 generation 切换，再评估低中断 A/B worker。
