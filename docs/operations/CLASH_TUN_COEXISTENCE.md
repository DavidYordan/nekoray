# Clash TUN 共存与本机诊断

状态：现行
最后更新：2026-07-24

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
6. 未经用户安排维护窗口，不停止 Clash TUN，不修改 Clash 持久配置，不改 Windows 系统代理、路由、DNS或防火墙。

## 可接受的临时绕过

- 进程级 socket/interface 绑定；
- 临时配置副本中的真实 IP、接口诊断字段或 DNS bootstrap 对照；
- OpenWrt 固定临时端口探针；
- 独立 Windows 测试机。

这些结果只能用于归因，不得提交为产品默认。若只有修改 Clash 持久配置、关闭 Clash TUN 或改系统路由才能继续，必须停止测试并申请维护窗口。
