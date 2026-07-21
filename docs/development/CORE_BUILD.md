# RouteFluent core 构建

状态：现行；交付产物尚未同步
最后更新：2026-07-20

当前代码和 Windows 构建链只集成 RouteFluent patched sing-box；这是现状，单核心是否作为长期产品约束仍由 ADR 0002 决定。

## 组成

```text
go/cmd/nekobox_core/
third_party/routefluent-sing-box/
third_party/libneko/
```

当前 wrapper 固定：

- sing-box `1.13.12`
- sing-anytls `0.0.11`
- RouteFluent 版本 `1.13.12-routefluent-anytls-client.7`
- patch id `routefluent-anytls-client-dns-resolver-group-check-v1`

manifest 至少必须包含：

- `anytls_outbound_client_field`
- `routefluent_dns_resolver_group`
- `routefluent_dns_check_start_validation`

## 推荐方式

使用完整 Windows 构建入口：

```powershell
.\build_windows_package.ps1
```

它会调用 `third_party/routefluent-sing-box/build_routefluent_sing_box.py` 准备受控源树，再构建 `nekobox_core.exe`。

`libneko` 已固定为 `third_party/libneko` 子模块提交；克隆后必须使用 `git submodule update --init --recursive`，不得再用仓库外同名目录覆盖构建输入。

## 必须验证

增量接管工作使用本轮构建目录中的 wrapper：

```powershell
.\build-package-windows64\nekobox_core.exe version
.\build-package-windows64\nekobox_core.exe check -c <config.json>
```

`check -c` 只证明 schema 和 pre-start 校验通过，不证明 Mixed 监听、DNS、TLS、AnyTLS 会话或真实连通。

截至 2026-07-20，`deployment/windows64/nekobox_core.exe` 仍是 2026-07-18 的旧产物；正式完整打包前不得用它代表当前源码。`run/check` 会直接读取 sing-box 配置，安全边界见 [CLI 文档](../reference/CLI.md#core-高级-cli)。

RouteFluent 自有测试和 fixtures 应进入 CI；主项目仍需单独覆盖 GUI、数据库、导入和运行状态。
