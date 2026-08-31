# 待确认决策

状态：只列无法由 [产品方向与开发契约](PRODUCT.md) 和 NekoRay 上游行为直接推出的问题
最后更新：2026-08-31

## 已冻结，不再作为选项

- 本项目是 NekoRay 4.0.1 的最小补强，不是新代理产品。
- 核心能力为：订阅 resolver 与人工多入口管理、AnyTLS、多线路并发端口。
- 原生 `127.0.0.1:2080` 保留 NekoRay 正常入口语义；新增专用端口才严格绑定线路。
- 入口与线路选择由用户明确操作，诊断不得自动改选。
- provider resolver 不回落本机/公网 DNS；专用线路端口不回落主线、其它线路或 direct。
- 专线入口不恒定优于公网入口，不做延迟择优、自动切换、round-robin 或智能路由。
- NekoRay 旧能力默认保留；只删除 Xray 运行核心及真正专属路径。
- MultiMapper、RouteFluent 是设计与 fixture 来源，不是可直接移植的产品架构。
- persistent Runtime、Windows service 和 WFP kill-switch 不是当前核心需求或发布前提。
- `server:port:user:pass` 导出直接使用 profile 原始 server（不是 profile 名称或诊断 IP），原样保留 IPv4、域名或主机名且不调用 DNS；没有转义规则前不导出含冒号的 IPv6。
- TCP Ping 保留为当前 Windows 网络路径的 server 可达性诊断，不冒充所选 proxy outbound 的 URL Test。
- 当前开发主机的 Clash TUN 必须始终运行；任何开发、测试和自动化都不得停止、重启、结束或改写它。Windows 专属 TUN 证据转移到隔离 Windows 环境，不能申请自动化停网窗口。

## 仍需用户体验层决定

| 优先级 | 问题 | 未确认时采用的保守实现 |
|---|---|---|
| P1 | 专用并发端口的默认建议范围与 UI 入口 | 保留现有数据但不把 `12100..12299` 冻结为产品契约；用户可见、可改，冲突时报错 |
| P1 | 同一 provider 集合中多个 DoH endpoint 如何展示 | 按订阅顺序列出并记录实际使用者；不跨 resolver 来源 |
| P1 | “命名入口集合”是否需要多 IP 运行策略 | 先只用于整理候选；运行时必须由用户固定单一入口 |
| P1 | 第一阶段允许哪些外置 core 参与并发托管 | 保留 profile；不支持的组合启动前精确拒绝 |
| P1 | Windows TUN 验收采用独立 VM、Windows 沙盒还是其它测试机，以及具体网络拓扑 | 未确认前只做静态/loopback/WSL/OpenWrt 分层证据并标记 Windows 未验证；推荐优先评估可快照回滚的独立 Windows VM，不创建环境、不改变宿主网络 |
| P2 | Windows 最低版本、ARM64、安装器 | 继续以当前 x64 便携环境验收，不作正式兼容承诺 |

## 工程问题，不再转嫁为需求问题

- AnyTLS + Trojan/front proxy 的 EOF 按兼容 bug 调查，不能据此删除 chain。
- 旧 Resolve Domain 暂停是止损；来源可见、保留原域名/SNI 的人工多入口管理仍须实现。
- external-core、Naive、分享格式、插件兼容等误删能力应以 `adef6cd` 为基线选择性恢复。
- 已加入的配置事务、RPC 生命周期与 Windows 运行时机制逐项按必要性审计，不能因 sunk cost 继续扩建。
