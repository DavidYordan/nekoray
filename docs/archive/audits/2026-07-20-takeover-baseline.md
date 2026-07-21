# 2026-07-20 接管基线审计

> 状态：历史证据；不得作为现行需求或当前通过证明。
> 归档日期：2026-07-20。
> 替代文档：[接管状态](../../TAKEOVER_STATUS.md)、[产品契约](../../PRODUCT.md) 与 [推进路线](../../ROADMAP.md)。
> 已知错误：部分实验强制 `auto_detect_interface=true`，不能代表产品接口策略。

源码调查基线：`96f1166`

## 归档理由

接管初期的 Mixed/AnyTLS 实验曾被同时复制到状态、已知问题、排障和测试矩阵，随后又因用户确认生产 TUN 必须保留而推翻了“强制自动检测物理接口”的临时方向。本文集中保存那组 dated evidence；现行需求和后续顺序分别以 `PRODUCT.md`、ADR 和 `ROADMAP.md` 为准。

## 当时的环境

- `D:\Program Files\nekoray` 中另一个 Nekoray 实例正在使用 `127.0.0.1:2080` 和 Wintun。
- 接管项目配置默认主 Mixed 为 `127.0.0.1:12080`。
- 当前 AnyTLS 所在 group 配置了 Trojan front proxy，因此主 AnyTLS outbound 带有 detour。
- 接管工作树曾临时把 `route.auto_detect_interface` 硬编码为 `true`，用于判断 Windows 默认 TUN 是否影响 outbound；这不是产品需求，后来已经撤销。

用户随后明确：上述已安装 Nekoray 是必须保持运行的生产网络，本项目不得停止或绕过它。因而文中“旧 TUN”统一解释为“受保护的外部生产 TUN”，任何要求关闭它的旧结论均已失效。

## 原始实验结果

| 条件 | HTTP | HTTPS CONNECT | SOCKS5h | 当时可得出的结论 |
|---|---:|---:|---:|---|
| 新 core + Trojan profile | 204 | 204 | 204 | Mixed parser、基础 DNS 和代理入口可工作 |
| AnyTLS，原生成逻辑 | 502 | 未完整复验 | 空响应 | 请求已进入 Mixed，失败在下游 |
| AnyTLS，仅临时取消 front proxy | 502 | 未完整复验 | 空响应 | Windows 默认路由条件仍在 |
| AnyTLS，临时强制接口检测并保留 front proxy | 502 | 超时 | 空响应 | detour 路径仍失败 |
| AnyTLS，临时强制接口检测并取消 detour（首次） | 204 | 204 | 204 | 该临时组合曾完成一次闭环 |
| 同一临时组合最终复验 | 502 | 0 | 0 | Mixed 仍监听；AnyTLS TCP 拨号超时，结果不可重复 |

因此唯一稳定结论是：表象不是“Mixed 根本没有监听或不能解析”；失败需要继续拆分为入口、逻辑线路映射、front proxy、DNS、TCP/TLS/AnyTLS 和底层接口。一次 204 不能证明节点稳定，也不能把绕过生产 TUN 固化为修复。

## 同日 OpenWrt 隔离复验

后续在 `192.168.1.7` 上使用与本项目相同版本的 patched sing-box，以独立 loopback Mixed、临时目录、临时端口和精确 PID 复验最终导出配置。探针未修改现有 RouteFluent 配置、服务、路由或防火墙。

| 候选配置 | HTTP | HTTPS CONNECT | SOCKS5h | 运行日志/结论 |
|---|---:|---:|---:|---|
| 主导出原样：`proxy` 为 AnyTLS `mihomo/1.19.28`，`detour=g-2` | 失败 | 失败 | 失败 | AnyTLS `failed to create session: EOF` |
| 同一 AnyTLS，仅移除 detour，保留 Mihomo client | 204 | 204 | 204 | AnyTLS 单跳三协议完成闭环 |
| 同一 AnyTLS，移除 detour并改为 native client | 失败 | 失败 | 失败 | 服务端报告 internal error |
| 独立 profile 2 Trojan | 204 | 204 | 204 | Trojan 单跳完成闭环 |

配置对象比较确认：主导出中的 `g-2` 与独立 profile 2 导出的 Trojan outbound 完全相同。结合日志可得出比早期 Windows 实验更强的结论：

1. Mixed listener 和最高优先级入口映射工作正常，请求确实到达目标 AnyTLS outbound。
2. Mihomo client 的 AnyTLS 单跳可用；native client 对当前服务端不可用。
3. Trojan 单跳及其 outbound 对象可用。
4. 当前隔离失败点是 AnyTLS + Trojan detour 的组合链兼容/握手语义；不能再把它笼统归为 Mixed、端口、Trojan profile 损坏或本机生产 TUN。

每轮探针前后，现有 RouteFluent 服务、配置、监听和连通基线保持不变；测试创建的临时进程、目录和端口均已清理。本机 `D:\Program Files\nekoray` 生产 Nekoray 从未参与或被修改。报告不保存服务器、认证信息或密钥。

该复验仍不能证明 Windows 系统代理、Wintun 接口身份、stacked TUN、WFP kill-switch、GUI 生命周期或进程交接正确。这些项目需要用户安排的本机维护窗口和 Windows 故障注入证据。

## 当时的接管产物

- Windows GUI 增量构建通过；记录的 GUI SHA-256 为 `ABFA18530EF03F2B6D6F0F5B391D5E5B165073DA38602B4910678168FE7883C9`。
- 配套 core SHA-256 为 `77D3F230271F9D930C3B63973D55724B1C0A1ABACBB7EFA9FB0BECF47312C793`；该 core 早于当轮 GUI 构建，不能视为同一完整发布产物。
- Trojan/AnyTLS 导出 JSON 的 `nekobox_core check` 通过。
- `test/test_mixed_probe.ps1` 的 direct、dummy auth 和安全拒绝 fixtures 通过。
- `test/test_runtime_connectivity.ps1` 能拒绝错误期望状态并核对临时 listener PID。
- CTest 没有发现 C++ 测试，自有 Go module 没有 `_test.go`，所以编译成功不是测试覆盖。

## 后续纠偏

1. 默认端口 `12080` 已由用户接受；`2080` 明确属于外部生产实例。
2. 标准生成配置已为主 Mixed 增加最高优先级 `mixed-in -> proxy` 绑定，与辅助端口一致。
3. Mixed-only 已恢复为遵循 Windows 系统路由；不再强制 `auto_detect_interface=true` 绕开外部 TUN。
4. OpenWrt 临时探针已完成首轮 AnyTLS client/detour A/B，并把故障收敛到 AnyTLS + Trojan detour 组合；Windows 专属行为仍须在用户安排的维护窗口验收。
5. 退出、重启和线路切换不得改系统代理/TUN，且不得直连；当前 GUI 直管 core 的架构被判定不合格，目标改为持久 Runtime Service + WFP。

归档中的旧计划如果建议“先关闭 TUN”“启动时自动恢复模式”或把死端口黑洞当作完成，均由现行 ADR 0004、0007、0008 取代。
