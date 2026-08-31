# NekoRay Windows 私人分支

这是基于 NekoRay 4.0.1 的 Windows 私人二次开发项目。目前处于接管整改期，**不可发布**；`deployment/windows64` 仅是本机审计部署。

## 产品边界

本分支遵循“最小化扩展”原则：NekoRay 原有能力默认保留；只有 Xray 运行核心因不支持 AnyTLS 被明确删除。核心新增需求有三项：

1. Clash 订阅的 server-domain resolver 与人工多入口管理：按字段存在性读取订阅 DoH，保留不同解析来源的候选入口，由用户手工固定；
2. AnyTLS 协议支持；
3. 多线路并发端口：每个新增专用 Mixed 端口精确绑定一条完整逻辑线路，失败时不改投其它线路。

原生 `127.0.0.1:2080` 仍是 NekoRay 的正常 Mixed 入口，不得用无条件规则遮蔽上游路由；严格线路选择由显式的专用并发端口承担。

另有一个边界清楚的私人导出便利功能：线路右键多选在保留上游含 remark 链接和 Neko Links 的同时，新增不含 URI fragment 的原生链接，以及 `server:port:user:pass` 四字段凭据列表。第一字段直接取 profile 原始 server，可原样保存 IPv4、域名或主机名，既不是 profile 名称，也不调用 DNS；完整边界见[产品契约](docs/PRODUCT.md#7-已明确的便利功能)。

旧能力只有在证明与上述扩展存在真实冲突后，才可以提出窄范围修改；不能因名称含 `v2ray`、依赖外置 core，或为了本机测试方便而删除、改写或硬编码产品行为。

## 冻结约束

- 当前首要验收平台是 Windows x64 私人便携构建；这不授权删除无冲突的上游能力。
- 默认主 Mixed 端口恢复为 NekoRay 原生值 `2080`；辅助端口的具体默认范围只是实现参数，不是产品原则。
- 本机基础网络现为 Clash TUN。它是不可中断的外部底层网络，必须始终运行；本项目和测试不得停止/重启其进程，也不得改写其配置、接口、DNS 或路由。后续只做 Windows 验证，相关网络测试应转移到本机创建的独立 Windows 沙盒/VM；共存和证据边界见 [Clash TUN 共存](docs/operations/CLASH_TUN_COEXISTENCE.md)。
- `auto_detect_interface` 只承担 NekoRay 原有的产品 TUN 防环路语义，不负责按 Mixed 端口选线路。测试环境绕行只能存在于显式、临时的测试副本中。
- 系统代理和项目 TUN 的手工操作应恢复并保持 NekoRay 上游语义；诊断、订阅刷新和无关配置操作不得擅自启停它们。GUI 退出、切线等具体行为先对照 `adef6cd`，不能以此反向要求 persistent Runtime/WFP。
- 受管 provider resolver 和专用并发端口必须失败关闭：不得回落到本机 DNS、主线路、`direct` 或其它可用线路。

## 当前判断

- Clash DoH 的字段存在性语义、strict provider resolver、AnyTLS 骨架、批量分享和辅助 listener 精确绑定思路可以保留并收敛。
- 2026-07-28 已移除原生 `2080` 到主 outbound 的无条件终结绑定，并加入导出回归断言；专用并发端口继续承担严格线路选择。
- 旧 **Resolve domain** 使用 Windows 系统 DNS 并永久覆盖域名，暂时禁用是合理止损；但来源可见、保留原域名/SNI、由用户固定入口的多入口管理仍然缺失。
- 2026-08-31 已纠正两项过度阻止：未解析 server 可原样导出四字段凭据格式；上游 TCP Ping 恢复为当前 Windows 网络路径的 server 可达性诊断，并明确不等同于 proxy outbound URL Test。
- 2026-08-31 已恢复 SOCKS legacy 分享链接的 base64 `user:password` 兼容；严格保留显式密码和无法无歧义解码的输入，不恢复 Xray runtime。
- 上一阶段误删了 external-core、Naive、部分分享/插件兼容、GeoSite 自动完成等上游能力，必须以 4.0.1 为对照选择性恢复。
- 大规模配置事务、RPC 生命周期、persistent Runtime/WFP 等改造并非三项核心需求；已有代码只按实际必要性审计，不能继续被当作发布前提扩建。
- MultiMapper 与 RouteFluent 只提供思想和测试材料：前者可参考来源/候选/固定入口，后者可参考显式端口/chain/fail-close；两者的完整产品架构都不移植。

完整证据见 [接管状态](docs/TAKEOVER_STATUS.md) 和 [偏离审计](docs/archive/audits/2026-07-20-scope-deviation-audit.md)。

## 构建

```powershell
.\build_windows_package.ps1
```

打包脚本发现目标部署目录仍有运行实例时会直接失败，不会关闭或强杀 GUI/core。详细依赖和限制见 [Windows 构建](docs/development/BUILD_WINDOWS.md)。

仓库保留过一次不带 Skip 参数的历史本地完整打包证据，但该 package 早于后续整改，不能代表当前 HEAD。本轮当前源码只完成增量 GUI/测试构建；deployment/zip 仍是被忽略的本地审计产物，不是 release manifest 或可发布包。

## 文档入口

- [Agent 工作规则](AGENTS.md)
- [产品契约](docs/PRODUCT.md)
- [接管状态](docs/TAKEOVER_STATUS.md)
- [现行整改队列](docs/KNOWN_ISSUES.md)
- [历史路线](docs/ROADMAP.md)
- [尚待确认的少量决策](docs/DECISIONS_NEEDED.md)
- [文档索引](docs/README.md)

## 上游与主要组件

本项目源自 NekoRay/NekoBox，并使用 Qt、gRPC、sing-box、anytls/sing-anytls、yaml-cpp 等组件。产品契约只允许 RouteFluent patched sing-box 承载 AnyTLS client 与 server-domain DoH；当前 fork 仍有已隔离但尚未删除的 fallback 实验残余，发布前必须清除。使用该 fork 不意味着推翻 NekoRay 的外置核心或协议模型。
