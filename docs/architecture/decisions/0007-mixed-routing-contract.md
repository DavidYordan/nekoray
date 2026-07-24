# ADR 0007：Mixed 端口到线路映射

状态：Accepted
日期：2026-07-20

## 决策

- 默认主入口 `mixed-in:2080` 绑定当前主 profile完整 outbound chain。
- 每个 `aux-mixed-<profile-id>` 绑定同一 profile的辅助 chain。
- 端口是逻辑线路选择器；`route.auto_detect_interface` 只处理底层接口/产品TUN防环路，不参与主/辅助选择。
- 绑定线路失败时该入口失败；禁止改投 `direct`、`bypass`、主线、另一辅助线、selector/urltest或其它可用性fallback。
- 空 chain、失效/归档 profile、非法或重复端口、循环front proxy和不支持组合必须使候选配置整体失败，不能跳过。

## 路由编译原则

可以在终结绑定前保留不会改投或泄漏的 NekoRay动作，例如必要的 sniff与明确reject。会发起解析或选择outbound的动作必须满足该线路自己的严格契约；辅助入口不能先借主/默认 DNS执行 `resolve`。

最终 custom合并后必须再次验证：

- inbound tag与port唯一且与持久配置一致；
- 每个受管入口在任何可能改投的规则之前终结到准确expected outbound；
- expected outbound唯一存在、不是direct/fallback类型；
- detour图不通向direct/bypass/block/其它线路；
- custom inbound不能抢占受管tag/port。

`internal-full`和暂不能参与并发的external-core是上游高级能力。无辅助并发时可在隔离模式保留；要求并发而无法证明契约时明确拒绝，不能删除能力。

## 物理接口

Mixed-only跟随Windows路由，包括本机必须保留的 Clash TUN。本项目产品TUN模式继续使用NekoRay原有 `auto_detect_interface=dataStore->spmode_vpn` 防环路。任何 Clash/Fake-IP/双TUN测试特例只能在显式临时副本中出现。

## 验收

主+至少两个辅助端口分别连接可观察的不同mock/真实出口；任一目标故障时另外两个出口与direct计数必须为0。负向测试覆盖custom覆盖、空链、端口冲突、DNS策略、front proxy 与候选启动失败；提交后故障必须继续阻断，只有用户明确选择且旧 generation 重验后才覆盖显式回滚。
