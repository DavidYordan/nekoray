# Clash TUN 共存与本机诊断

状态：现行
最后更新：2026-08-31

## 不可中断约束

本机网络依赖 Clash TUN。它必须始终保持运行，不存在由 agent、脚本或本项目自行开启的停用窗口：

- 不停止、不重启、不结束 Clash 进程；不按进程名、端口、接口或模糊条件清理它；
- 不修改 Clash 持久或临时配置，不接管其控制端口、虚拟接口、Fake-IP、DNS、路由和防火墙状态；
- 不为了启动本项目 TUN、构造双 TUN 对照或取得“更干净”的出站路径而改变上述状态；
- 只能采集必要的脱敏只读证据。若保持 Clash 运行会使某项断言无法成立，结论是“当前主机未验证”，下一步是转移到隔离环境。

## 产品边界

本项目默认主 Mixed 为 `127.0.0.1:2080`。Clash TUN 是本机外部底层网络，不属于本项目的运行时，也不是线路 fallback。产品配置不得为了这台机器硬编码 Clash 的接口名、Fake-IP 网段、物理网卡、LAN DNS 或 `auto_detect_interface=true`。

Mixed-only 模式按 Windows 当前路由发送底层连接。如果 Clash 处于 TUN + global 模式，项目 core 的连接可能再次进入 Clash 所选代理；这是一条“项目代理套在 Clash 代理里”的链，不等于绕过 Clash。目标节点或中间链不接受该路径时，Mixed 入口仍能监听并命中 `proxy`，但出站会返回 EOF/超时。

项目自身 TUN 使用接口自动检测避免捕获自己的底层连接。在 Clash Fake-IP 模式下，Windows 系统 DNS 可能返回 `198.18.0.0/15` 地址；若项目 core 随后把该地址绑定到物理接口发送，Fake-IP 在 Clash 之外不可达。此时表现为 DoH bootstrap 或节点域名解析超时。这是双 TUN/Fake-IP 的环境冲突，不能用产品硬编码修复。

## 2026-07-24 本机证据

- Clash 当前为 TUN、Fake-IP、global 模式，默认虚拟接口为 `Meta`。
- 项目临时 core 能监听 Mixed；HTTP、CONNECT、SOCKS5h 都命中 `proxy`，所以入口和端口映射不是这次失败点。
- 不绕过 Clash 时，WD 的 Trojan TLS 被对端关闭；NEX 的 AnyTLS 经同一 Trojan front proxy 后出现 session EOF。
- 仅对一次进程内 TLS 探针绑定物理接口并使用 LAN DNS 得到的真实节点 IP时，目标 TLS 握手成功。该对照没有修改 Clash、系统代理、路由或产品配置，证明 Clash 接管路径会改变结果。
- `192.168.1.7` 当时没有 ARP 响应，SSH 无法读取 banner，所以 OpenWrt 对照未能执行；不得把这次超时写成线路失败。

## 安全诊断顺序

1. 核对 `2080` 的 listener PID 和路径确属当前 package。
2. 从目标 profile 导出配置并执行 `nekobox_core check`。
3. 用收紧后的临时 Mixed 副本分别测试 HTTP、CONNECT、SOCKS5h；不得启动 TUN或写系统网络状态。
4. 若日志已命中目标 `proxy`，按 DNS bootstrap、TCP、TLS、协议、detour 分层，不再归咎 Mixed。
5. 需要绕过 Clash 时，只允许使用显式、一次性的诊断覆盖，并在报告中标记；优先使用独立 Windows 环境或可达的 OpenWrt 实验机。
6. 当前主机不以停用 Clash、修改其配置或改 Windows 系统代理、路由、DNS、防火墙作为排障步骤；无法归因时停止本机实验并转移环境。

## 隔离环境与证据范围

| 环境 | 可以证明 | 不能据此宣称 |
|---|---|---|
| 当前 Windows 主机（Clash 保持运行） | 静态检查、构建、纯逻辑、loopback、只读 PID/端口/路由快照，以及“请求在当前叠加网络中完成” | 本项目 TUN 独立行为、物理出站归属、无双 TUN/Fake-IP 干扰 |
| WSL2 | Linux core、parser/config/schema、协议 loopback、无 Windows 副作用的分层诊断 | Windows Wintun、系统代理、WFP、网卡/驱动、GUI 生命周期；WSL 远端流量仍受宿主网络影响 |
| OpenWrt 临时探针 | 相同 core 的 schema、DNS、协议、detour 和远端出站 | 任何 Windows 专属行为 |
| 独立 Windows VM、Windows 沙盒或其它测试机 | 在快照和精确进程所有权下验证 Wintun、系统代理、GUI/core 生命周期、Windows 路由/DNS | 未实际覆盖的物理机驱动、休眠、多网卡等场景 |

创建隔离环境是独立工作包。开始前应向用户提交：平台和版本、与宿主的网络隔离方式、是否需要管理员权限、快照/回滚、测试包来源、允许创建的进程/接口，以及精确清理方法。不得通过改变宿主 Hyper-V/vSwitch、默认路由、DNS、系统代理或 Clash 来“隔离”。

## 可接受的临时对照

- 进程级 socket/interface 绑定；
- 临时配置副本中的真实 IP、接口诊断字段或 DNS bootstrap 对照；
- OpenWrt 固定临时端口探针；
- 独立 Windows 测试机。

这些结果只能用于归因，不得提交为产品默认。若只有修改 Clash、关闭 Clash TUN 或改宿主系统路由才能继续，必须停止当前主机测试；没有合格隔离环境时明确记录未验证，不得请求自动化停网，也不得以 WSL/OpenWrt 结果替代 Windows 验收。
