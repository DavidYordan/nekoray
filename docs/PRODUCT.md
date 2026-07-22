# 产品契约

状态：现行、已冻结部分为开发硬约束
调查基线：NekoRay 4.0.1 `adef6cd` → 偏离审计终点 `96f1166`；后续整改状态以接管文档和 Git 历史为准
最后更新：2026-07-22

## 1. 产品定位

- 仅支持 Windows；当前 x64 便携构建是实现事实，最低 Windows 版本和未来架构仍待定。
- 纯私人项目，不维护公共发行、Linux/macOS、AUR、社区、捐赠或公开更新渠道。
- 本项目是 NekoRay 的窄范围二次开发，不是重新设计一个代理客户端。

## 2. 最小化二次开发原则

NekoRay 4.0.1 的既有协议、数据模型、导入/导出、路由、测速、外置 core、Naive 和 UI 能力默认保留。明确允许删除的是 Xray 运行核心及真正只服务于 Xray 的路径，因为 Xray 不支持 AnyTLS。

以下理由单独都不能授权删除能力：

- 名称中出现 `v2ray`；
- 能力依赖外置可执行文件；
- 当前三项扩展尚未用到；
- 本机同时运行另一套 NekoRay/TUN，导致测试不方便；
- 现有代码已经被上一任删掉。

若旧能力与新扩展发生真实冲突，必须先提供复现、影响范围、兼容方案和数据迁移方案，再做最小修改。暂时无法并发承载的组合应明确拒绝运行，不能删除 profile、静默降级或改投其它线路。

## 3. 三项产品扩展

### 3.1 AnyTLS

- NekoRay 必须能新建、保存、编辑、导入/导出并运行 AnyTLS profile。
- 兼容字段必须显式区分 native、Mihomo compatibility 与继承来源，不能仅凭“来自 Clash”永久推断任意节点身份。
- NekoRay 原有 front proxy/chain 思想默认继续成立；当前 AnyTLS + Trojan detour 的 EOF 是待修复兼容问题，不是删除 front proxy 的理由。

### 3.2 多线路并发

- 默认主 Mixed 为 `127.0.0.1:12080`，绑定当前主 profile 的完整 outbound chain。
- 每个辅助 Mixed 端口绑定创建它的辅助 profile 完整 chain。
- 端口决定逻辑线路；`auto_detect_interface` 只影响底层接口选择，不能在主/辅助线路之间“自动检测”。
- 线路不可用时该端口必须失败，绝不能回落到主线、另一辅助线、`direct`、`bypass` 或其它可用性 fallback。
- 不支持并发托管的上游高级/外置 core 组合应在启动前给出精确错误；上游能力和旧数据仍须保留。

### 3.3 Clash server-domain DoH

- 只读取明确的 `dns.proxy-server-nameserver` 或 `dns.proxy_server_nameserver`，用于解析 proxy `server` 域名。
- 普通 `dns.nameserver` 不是它的替代项，不能在缺失或非法时自动借用。
- 对已经绑定 provider DoH 的 server-domain resolver，DoH 失败必须失败关闭；该受管 resolver 不得借本机 DNS、主线或其它线路 fallback。这里不把“没有 provider DoH 的普通 NekoRay 节点”误写成已具备相同的自定义 strict resolver 语义。
- DoH endpoint 自身为域名时需要独立、可审计的 bootstrap 策略，不能把本机 DNS/直连隐藏成“完全零泄漏”。在该策略实现前，过渡生成器只接受 URL host 已是 IP 的 DoH endpoint；域名 endpoint 明确构建失败。
- 没有 provider DoH 的普通 NekoRay 节点继续使用上游正常解析路径，不应被强制套入本项目自定义 resolver group。

## 4. Windows 网络模式不变量

- 只有用户明确触发、目标精准、结果可核对的手动操作可以启用/停用系统代理或 TUN。
- GUI 退出/重启、core 重启/崩溃恢复、线路切换、订阅刷新和辅助端口变更不得改变系统代理/TUN 的 OS 模式。
- TUN 开启时必须允许切线或重启线路，不能要求用户先关 TUN。
- 切换失败允许可观察的全阻断；IPv4、IPv6 和 DNS 均不得回落直连。
- UI 状态必须来自实际 Windows 状态、精确进程身份、监听与 generation，不能把持久化意图当成成功。

现有单个 sing-box `Stop -> Start` 会卸载内部 TUN，且没有独立 WFP 保护，尚不满足以上契约。当前止损仅包括：内部 TUN 活动配置强制 strict+IPv4/IPv6；最终配置要求产品 TUN listener 与完整生成对象一致、保留生成的接口自动检测策略并拒绝已知 bind/default-interface 覆盖；同时拒绝未授权 TUN、任何 inbound `set_system_proxy=true`、系统 WireGuard/Tailscale endpoint 与 NTP 写系统时钟。自动提权续开已删除，legacy Windows 系统代理 UI 切换暂禁。

UI 中 `spmode_vpn` 仅表示用户期望，`running_internal_tun`/外置 worker PID 仅表示当前 worker 的观测，二者都不是 Windows OS 事实源。core 崩溃后当前实现只重启空控制 core，保留“请求开启”的 UI 意图但不自动恢复 profile/TUN；这避免隐式启用，却不能维持崩溃窗口保护。这些止损都不是持久运行时的替代品；候选架构见 [ADR 0008](architecture/decisions/0008-persistent-windows-runtime.md)。

Windows GUI 当前忽略 CRT `SIGTERM`/`SIGINT`，只用于防止控制台信号绕过受保护的 UI 退出路径；它不覆盖强制结束进程、崩溃、系统关机或 worker 自退。最终契约仍要求由独立于 GUI/worker 的 Runtime/WFP 层维持 OS 模式和 fail-closed 数据面。

## 5. 测试环境边界

- `D:\Program Files\nekoray`、`2080` 和其生产 TUN 永远是 no-touch 外部状态。
- Mixed-only 产品配置应遵循 Windows 当前路由。不得为绕过本机生产 TUN，在产品生成器中硬编码 `auto_detect_interface=true`、物理网卡名、路由标记或其它本机特例。
- 临时诊断覆盖必须默认关闭、只改临时副本、在报告中显式记录。
- 当本机生产 TUN 使协议归因不清时，使用 `192.168.1.7` OpenWrt 隔离实验室验证相同 core 的配置和 outbound；它不能替代 Windows Wintun/WFP/生命周期验收。

## 6. 数据安全

- 未知、旧版或损坏配置必须保留/隔离并提示，不得静默删除。
- 保存和订阅刷新必须先完整 parse/validate/stage，再提交；任何失败保持旧数据不变。
- 运行引用、profile ID、group order、front proxy、chain 和辅助端口必须一致，禁止悬空引用或 ID 静默复用。
- 密码、订阅正文和完整导出配置不得写入普通日志或提交。

## 7. 运行入口与信任边界

- 普通 GUI 路径由 C++ ConfigBuilder 生成并校验产品配置，再通过仅监听 localhost、使用每个 GUI session 随机令牌的 gRPC 控制 core；每次 core 启动另有 UUID，所有 RPC 先验证精确实例身份，日志后也必须完成 UUID/协议握手才 ready。Start/Stop/Exit 使用单调 command sequence；Start/Stop 响应不确定时可用同一 daemon mutex 内的更高序号屏障对账。UUID/对账只证明进程内记录，不是配置授权、持久 runtime generation 或 Windows OS 状态。
- `nekobox_core run/check` 是用户显式选择的高级 CLI，供构建、审计和经过收紧的隔离测试使用；它能够直接读取 sing-box 配置，因此不得被描述为普通 GUI 的任意绕过，也不得用于未经审计的配置。
- 当前 Go core 会执行 sing-box 自身的配置与生命周期处理，但尚未重复执行 C++ 层的 Mixed、TUN、系统代理和 resolver 产品策略。该纵深防御缺口必须在受控 core/Runtime 边界补齐；随机令牌不能替代配置授权。

## 8. 不属于产品扩展的上一阶段新增项

MultiMapper 专用导出、复杂的批量域名解析/改 IP 平台、通用 resolver 健康探测平台等不在三项需求内。MultiMapper 与复杂批量 resolver/change-IP 实现已从产品代码移除，有历史价值的材料放入 `docs/archive/`。上游简单 **Resolve domain** 也暂时禁用：旧实现直接使用 Windows 系统 resolver，并把节点域名永久改成 IP，会绕过或破坏订阅的 `proxy-server-nameserver` 语义。当前 UI 入口只显示无副作用说明；未来若恢复，只能经对应 provider resolver，并保留原始域名语义。

## 9. 完成定义

必须区分：源码存在、能够构建、core schema 通过、真实 outbound 闭环、Windows 集成安全和最终验收。OpenWrt 成功只能证明相同 core 的配置/协议层；Windows 系统代理、TUN、WFP、重启和实例所有权必须在 Windows 单独取证。
