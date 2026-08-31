# ADR 0004：Windows 运行时与网络模式安全不变量

状态：Partially Superseded by [产品方向与开发契约](../../PRODUCT.md)
日期：2026-07-20
最后复核：2026-08-31

## 背景

系统代理和 TUN 是 Windows OS 状态，不等同于 GUI 中保存的布尔意图。当前 GUI、sing-box worker 和内部 TUN 生命周期耦合；只在退出时保留注册表或状态标记，会形成指向已停止入口的黑洞，而整核重启内部 TUN 还可能产生默认直连窗口。

本 ADR 曾把全局 Windows 数据面生命周期解释为冻结要求。2026-07-27 后，只保留“系统代理/TUN 由用户明确操作、受管 provider resolver 与专用线路端口不 fallback”等最小语义；独立数据面、WFP 或热切换不再是核心发布门。候选历史见 [ADR 0008](0008-persistent-windows-runtime.md)。

## 现行有效部分

- 系统代理和项目 TUN 的用户手工能力默认按 NekoRay 4.0.1 保留；诊断、订阅刷新和无关配置操作不得擅自启停系统网络模式。
- 启动时可以观察并展示实际 OS/worker 状态，但不能把持久化意图、复选框或一次 RPC 成功当作接口、路由或监听已经成立。
- 任何停止、恢复或清理操作只允许作用于本实例精确拥有且已核对的 PID、句柄、路径、接口和配置；不得按进程名或模糊范围处理其它安装。
- 该条关于 `D:\Program Files\nekoray` 占用 `2080` 的环境事实已由 [ADR 0012](0012-restore-native-mixed-port.md) 取代；当前外部受保护状态是不可中断的本机 Clash TUN，不得被本项目停止、恢复、改写或接管。
- GUI 退出、切线、项目 TUN 启停和系统代理写入的具体产品行为应对照 `adef6cd` 与现行产品契约逐项恢复/审计；本 ADR 不能自行扩大上游要求。

## 已被取代的历史扩展决定

上一阶段曾进一步要求：GUI 退出后数据面继续存在、TUN 切线期间全机 IPv4/IPv6/DNS 绝不直连、故障进入持久全阻断、由 compare-and-restore broker 管理系统代理，并建立独立 Runtime/WFP。上述要求需要稳定 anchor、persistent service/WFP 和新的 OS 状态机，已被 [产品方向与开发契约](../../PRODUCT.md) 明确排除为当前核心需求和发布前提。

因此：当前实现没有 persistent kill-switch 不是产品缺陷；内部 TUN 整 Box 重载、退出和切线 guard 是否退化必须分别对照上游复现；不能用本 ADR 为这些 guard 背书，也不能继续实施 [ADR 0008](0008-persistent-windows-runtime.md)。

## 当前实现与验收含义

普通 GUI 的 localhost 鉴权 gRPC 与用户显式执行的高级 `nekobox_core run/check` 是不同入口。高级入口读取 raw config 不等同于普通 GUI 绕过产品生成策略；只有证明产品启动链存在具体绕过后，才在最窄边界补重复校验。

现行 Windows 验收关注：手工系统代理/TUN 能力不比 `adef6cd` 退化；状态展示不把 requested 冒充 observed；退出/切线不会误杀其它进程或损坏数据；当前主机 Clash 始终保持不变。项目 TUN、系统代理和 Windows 路由行为必须在独立 Windows 隔离环境验证。全机无泄漏、BFE/service crash、persistent WFP 和 GUI 退出后持续数据面只在用户未来重新授权该方向时另立契约与验收门。
