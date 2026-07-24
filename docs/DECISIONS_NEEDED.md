# 待确认决策

状态：只列无法由现有要求和代码事实直接推出的问题
最后更新：2026-07-24

## 已冻结，不再作为选项

- 仅 Windows、纯私人项目。
- NekoRay 既有能力默认保留；Xray核心明确删除。
- 新增仅 AnyTLS、多线路并发、Clash proxy-server-nameserver DoH。
- 主 Mixed默认已按 ADR 0012 恢复为原生 `2080` 并绑定主线路；辅助 Mixed是并发线路需求的一部分，并绑定各自线路。
- 测试机/双 NekoRay特例不得进入产品默认；`auto_detect_interface` 不选择逻辑线路。
- 只有用户手动操作可启停系统代理/TUN；重启、退出和切线不改变 OS模式。
- 线路故障不得 fallback到 direct、主线或其它线路；TUN下不得要求先关 TUN。
- DoH endpoint 域名由 NekoRay 原生 `dns-local` bootstrap；它不等于 provider DoH 失败后的线路 server 本机 DNS fallback，后者仍禁止。

## 仍需用户体验/环境层决定

| 优先级 | 问题 | 未确认时采用的保守实现 |
|---|---|---|
| P0 | TUN切线是否允许短暂全阻断并重置现有 TCP/UDP会话，还是必须保持连接无感 | 允许可观察的短暂阻断与连接重置，但绝不直连；不宣称无缝 |
| P0 | WFP kill-switch保护全机还是本项目/指定进程；DHCP、LAN、RDP、打印机等例外范围 | 先做威胁模型和可回滚原型，不写入生产持久规则 |
| P1 | Windows重启后如何维持 WFP 阻断、展示残留/期望状态并引导用户手动重新启用 TUN | 不自动启用 TUN；保护层保持阻断或进入 `BLOCKED_NEEDS_USER`，只读展示并等待手动操作 |
| P1 | Windows最低版本、x64/ARM64、便携或安装器 | 继续按当前 x64便携开发，不写成正式支持承诺 |
| P1 | Allow LAN是否保留，以及辅助入口是否继承主认证 | 保留上游数据能力但不扩展暴露；正式验收前默认 loopback |
| P1 | “绝不直连”是否也覆盖订阅下载控制面；上游默认 `sub_use_proxy=false` 会直接请求订阅 URL | 不把它误称为线路 fallback；在冻结策略前保留上游行为并明确提示，绝不在代理失败后自动改走直连 |

## 工程问题，不再转嫁为需求问题

- AnyTLS + Trojan detour默认应继续支持 NekoRay front proxy思想；当前 EOF先按 bug调查。只有证明确实不可跨越时，才提交窄限制和迁移方案。
- external-core/Naive必须恢复数据与上游能力；某组合不能参与并发时明确拒绝即可。
- 首次导入是否展示差异可以按安全默认实现，不影响“失败零变更”硬约束。
- MultiMapper不是待确认核心需求，而是未经授权的新增项；产品 UI/CMake 入口已移除，历史材料归档。
