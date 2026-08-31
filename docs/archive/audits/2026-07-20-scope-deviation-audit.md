# 2026-07-20 范围偏离审计

> 状态：接管审计证据；结论已同步到现行 [产品契约](../../PRODUCT.md) 与 [已知问题](../../KNOWN_ISSUES.md)。
> 归档日期：2026-07-20。替代/现行入口：[接管状态](../../TAKEOVER_STATUS.md) 与 [推进路线](../../ROADMAP.md)。
> 审计范围：NekoRay 4.0.1 `adef6cd` 到 `96f1166`，以及 2026-07-20 接管工作树。
> 本文不含凭据、订阅正文或完整节点配置。

## 1. 总体规模

`adef6cd..96f1166` 共 36 个提交，涉及约 117 个文件，约新增 8,497 行、删除 3,236 行。改动并非“三个小扩展”，而是同时触及协议模型、核心包装、导入/导出、路由、DNS、测速、生命周期、打包、文档和 UI。

主要提交序列：

- `958f02b`、`c4aad4e`：AnyTLS 与 patched core 基线；
- `f36677c`、`11ba168`：以“清理”为名删除路由/格式兼容；
- `d83785e`、`b728b51`、`a24830d`、`72a06f4`、`deedd13`：Clash AnyTLS/DoH 与大量 resolver 扩展；
- `92327af`、`058919f`、`04df42a`、`1cd9a2a`：辅助 Mixed 并发；
- `b746dd7`、`b977aac`、`d1ff410`、`4997422`：未经需求授权的 MultiMapper；
- `0b6688b`、`0ed9274`、`96cc951`：把上游简单域名解析扩展成复杂批量工具；
- `45ca2e5`、`cf1e490`：用阻止 TUN 重载规避无泄漏问题；
- `d385a17`、`844d9b2`、`4d68e93`、`96f1166`：错误收敛为 sing-box-only 并删除外置 core/Naive。

## 2. 按产品边界分类

### A. 应保留并收紧：三项扩展

| 扩展 | 已存在 | 主要缺口 |
|---|---|---|
| AnyTLS | Bean、UI、链接、Clash 导入、outbound、patched client 字段 | client 继承不保真；非法值静默退化；AnyTLS + Trojan detour EOF；测试不足 |
| 多线路并发 | `12080` 主入口、profile→辅助端口、主/辅助链生成 | 空链曾可落入 `route.final`；custom 可打穿；状态非事务；TUN 下禁止重载 |
| Clash server-domain DoH | 提取、组/节点继承、outbound `domain_resolver`、patched resolver | 错拿普通 nameserver；无 DoH也强制自定义 group；本机 fallback/探测过度；bootstrap 未定义 |

### B. 应恢复：被误删的 NekoRay 能力

1. **external-core 与 Naive**（`d385a17`、`844d9b2`、`4d68e93`、`96f1166`）
   - `ExternalBuildResult`、`NeedExternal/BuildExternal`；
   - 外部进程启动/测速/链式 mapping；
   - ExtraCore 设置与 custom external core；
   - Naive Bean、链接、编辑 UI 与执行；
   - TUIC/Hysteria2 `forceExternal` 路径。

2. **不依赖 Xray 的兼容格式**（`11ba168`）
   - v2rayN VMess 分享格式；
   - SOCKS userinfo 兼容；
   - Shadowsocks v2ray-plugin Clash 导入与 UI；
   - 旧分享格式选项。

3. **路由与测速能力**
   - `f36677c` 删除 GeositeReader 后保留了空自动完成 UI；应针对现用 `.db` 格式重建或明确显示不可用；
   - `0be1762` 删除整组 URL Test，没有需求依据，应恢复等价入口。

恢复原则：选择性接回当前架构；Xray 继续删除。某能力若尚不能参与并发，应保留数据/UI并在该运行组合中明确拒绝，不能再次静默删除。

### C. 应退出产品主线：无需求新增

- `sub/MultiMapperExport.*`、专用 UI 与 YAML/JSON 契约；
- 复杂的批量域名解析/比较/替换 IP 工作流（应恢复上游简单工具或独立为只读诊断）；
- 为没有 provider DoH 的全部域名节点强制创建 `local_only` 自定义 resolver group；
- DoH 故障自动本机 fallback、公共探测域名与恢复状态机，除非后续有独立需求。

历史说明保留在 `docs/archive/`，但不能因为代码已经存在就升级为需求。

## 3. 实际安全缺陷

### Mixed/路由

- 标准生成路径已能做到主/辅助端口绑定准确；`auto_detect_interface` 与此无关。
- 空 chain、失效辅助 profile/端口曾被静默跳过，可能留下无绑定入口并落入主线或 direct。
- `custom_config`、`custom_outbound`、`custom_inbound` 和 `internal-full` 可覆盖最终不变量；需要最终配置 validator。
- 辅助入口先执行无指定 server 的 `resolve` 时会借用主/默认 DNS；严格辅助线路应保留域名给自己的 proxy chain或使用专属 resolver。

### 运行时

- 当前 Go `Start` 拒绝已有 instance，`Stop` 会关闭整个 sing-box Box；没有原地 reload RPC。
- 内部 TUN、Mixed、DNS 和 outbounds 在同一 Box，线路切换会卸载 TUN。
- 现有 UI guard 只是要求用户先关 TUN，直接违反冻结需求。
- 没有独立 WFP kill-switch 时，单进程崩溃或 Stop/Start 无法证明无直连。
- 辅助端口 desired map 在 reload 成功前保存，失败后 UI 与真实 listener 可能相反。

### 数据

- `JsonStore::Save()` 直接 truncate，未检查 open/write；崩溃或磁盘错误可能留下空文件。
- loader 对未知/损坏 profile 有直接删除路径；Naive 被误删后风险更高。
- 旧订阅刷新先改 group/order、甚至先删 profile，再知道解析结果是否有效。

## 4. `auto_detect_interface` 专项结论

NekoRay 上游已经使用：

```cpp
{"auto_detect_interface", dataStore->spmode_vpn}
```

它表示本项目产品 TUN 模式（内部或伴随进程）的通用防环路策略；Mixed-only 为 `false` 并遵循 Windows 路由。接管阶段曾为本机双 TUN 测试硬编码 `true`，该产品改动已撤销。

测试工具允许显式、默认关闭的临时覆盖，但必须记录。OpenWrt 探针曾无条件强制 `true`，现已改为默认 preserve，只有 `--force-auto-detect-interface` 才覆盖；旧远程结果必须标明它测试的是临时变体，不能反推产品默认。

## 5. 已完成的第一批止损

- 恢复 `auto_detect_interface=dataStore->spmode_vpn`；
- Clash API 默认保持关闭（配置值 `-9090`，启用时端口 `9090`），不因本机双实例改成 `19090`；
- server-domain DoH local fallback 默认关闭，辅助 chain 强制 strict；
- 无 provider DoH 时不再强制套用 RouteFluent `local_only` group；
- 空/失效辅助 chain、重复受管 Mixed 端口改为构建失败，listener 仅在 chain 成功后加入；
- 辅助 Mixed 不再先通过主/默认 DNS执行入口 `resolve`；
- 打包发现运行实例时 fail-fast，不再自动关闭/强杀；`ReferenceDir` 默认空，下载 fallback 空路径错误已修复；
- OpenWrt 探针默认保留导出配置的接口策略；对应单测已通过。

这些止损不等于项目已安全。external-core/Naive 与格式兼容尚未恢复，订阅和最终配置事务仍在整改，Windows 持久 runtime/WFP 尚未实现。

## 6. 推进结论

正确顺序是：先保护数据并恢复误删的 NekoRay 能力；再把三项扩展收敛为明确契约；最后建立 Windows 持久数据面与 fail-closed generation 切换。不能继续新增第四项产品功能，也不能用要求用户关 TUN、自动 fallback 或本机特例来掩盖架构缺口。
