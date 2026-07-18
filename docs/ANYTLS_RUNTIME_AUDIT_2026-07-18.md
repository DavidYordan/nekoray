# AnyTLS 运行时审计记录

日期：2026-07-18

## 结论

当前测试版生成的 AnyTLS core 配置已满足 RouteFluent patched sing-box 的要求：

- AnyTLS outbound 写入 `client=mihomo/1.19.28`。
- AnyTLS server 域名绑定了 `domain_resolver`。
- DNS servers 中包含 RouteFluent provider resolver group 和订阅 DoH。
- `nekobox_core.exe check -c` 对真实运行配置返回 0。

同一测试环境下 Trojan 基线可用，而当前 AnyTLS 节点在 core 创建 AnyTLS session 阶段返回 EOF。需要注意：当前机器同时运行另一套 Nekoray 的 TUN 模式，它可能显著影响本项目实例的出站网络路径，因此 EOF 只能作为本机观测记录，不作为 AnyTLS 支持是否完成的阻塞结论。

本轮审计重点收敛为：最终 core 配置是否正确、`nekobox_core.exe check -c` 是否通过、URL 测试配置是否仍使用 sing-box 已废弃/拒绝的 DNS 写法。实际连通性以后以用户在干净或明确绕过 TUN 的环境中手动测试为准。

## 已验证项

使用新增脚本导出 profile 66 的最终运行配置：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\export_profile_core_config.ps1 -ProfileId 66 -Check
```

结果：

- core check：通过。
- inbounds：1。
- outbounds：4。
- `proxy` outbound：`type=anytls`，`client=mihomo/1.19.28`，带 `domain_resolver`。
- 当前分组存在前置代理，因此 `proxy` outbound 还有 `detour=g-2`。

使用同样方式导出 Trojan 基线 profile 2：

- core check：通过。
- 纯 core 运行后，通过 `socks5h://127.0.0.1:12080` 请求 `http://cp.cloudflare.com/` 成功。

使用导出的 AnyTLS 配置直接启动 core：

- `mixed-in` 正常监听 `127.0.0.1:12080`。
- curl 请求进入 core。
- 失败点为 `outbound/anytls[proxy]` 创建 session。
- 错误为 `failed to create session: EOF`。

## 对照测试

做过临时、已回滚的配置对照：

1. 保留当前前置代理，但 AnyTLS 不写 `client`，仍 EOF。
2. 移除前置代理且 AnyTLS 不写 `client`，仍 EOF。
3. 保留订阅默认 `client=mihomo/1.19.28` 且移除前置代理，仍 EOF。

因此，当前 EOF 不能简单归因于 `client=mihomo/1.19.28` 或前置代理链路；同时也不能反向证明节点或服务端一定有问题，因为本机外部 TUN 仍可能改变网络路径。

## 已修复问题

`BuildConfig(forTest=true)` 以前没有写入 `route.default_domain_resolver`。在 sing-box 1.13.12 下，URL 测试配置会被 core 拒绝：

```text
missing route.default_domain_resolver or domain_resolver in dial fields
```

已调整 `db/ConfigBuilder.cpp`，让测试配置也写入：

```json
"default_domain_resolver": { "server": "dns-direct" }
```

重新导出 profile 66 的 `-ForTest` 配置后，`nekobox_core.exe check -c` 返回 0。

## 下一步建议

1. 不再把当前机器上的 EOF 作为优先排障项。
2. 用户后续手动实测时，优先确认实际运行环境是否关闭或绕过另一套 Nekoray TUN。
3. 若干净环境中仍出现 AnyTLS 不通而 Mihomo 可通，再抓取 Mihomo 对同一节点的最终 outbound 配置，比较 AnyTLS handshake 相关字段。
4. 本项目后续可继续增强 UI 诊断：右键线路导出最终 core 配置摘要、运行 `check -c`，并提示运行时 EOF 在存在外部 TUN 时不具备单独定性价值。
