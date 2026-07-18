# AnyTLS 运行时审计记录

日期：2026-07-18

## 结论

当前测试版生成的 AnyTLS core 配置已满足 RouteFluent patched sing-box 的要求：

- AnyTLS outbound 写入 `client=mihomo/1.19.28`。
- AnyTLS server 域名绑定了 `domain_resolver`。
- DNS servers 中包含 RouteFluent provider resolver group 和订阅 DoH。
- `nekobox_core.exe check -c` 对真实运行配置返回 0。

同一测试环境下 Trojan 基线可用，而当前 AnyTLS 节点在 core 创建 AnyTLS session 阶段返回 EOF。因此，当前问题不再指向本地 mixed 入站、Windows 系统代理、生产版 TUN、GUI gRPC 启动流程、RouteFluent `client` 字段名或订阅 DoH 绑定。

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

因此，当前 EOF 不能简单归因于 `client=mihomo/1.19.28` 或前置代理链路。

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

1. 从同一订阅中选择多条 AnyTLS 节点重复运行时测试，确认是单节点失效还是该订阅的 AnyTLS 全部失效。
2. 若多个 AnyTLS 节点均 EOF，需要进一步对比 provider 原始 Clash YAML 中的 AnyTLS 字段，重点看 `sni`、`alpn`、端口、password、skip-cert、idle session 参数是否存在 provider 特殊要求。
3. 若 Mihomo 客户端实际可用而本项目不可用，需要抓取 Mihomo 对同一节点的最终 outbound 配置，比较 AnyTLS handshake 相关字段。
4. 本项目后续可继续增强 UI 诊断：右键线路导出最终 core 配置摘要、运行 `check -c`、并提示 EOF 更可能是线路侧或协议握手问题。
