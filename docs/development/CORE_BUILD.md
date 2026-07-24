# RouteFluent core 构建

状态：现行；尚无可交付产物
最后更新：2026-07-22

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

它会调用 `third_party/routefluent-sing-box/build_routefluent_sing_box.py` 准备受控源树，再构建 `nekobox_core.exe`。无 Skip 流程还会用同轮 GUI 测试程序和刚构建的 package core 依次运行 tracker、分享格式、resolver policy 纯测试与 raw QProcess/Qt HTTP/2 Exit，全部通过后才创建正式 zip；任一 Skip 只产诊断 package 目录且不创建/覆盖 archive。

`libneko` 已固定为 `third_party/libneko` 子模块提交；克隆后必须使用 `git submodule update --init --recursive`，不得再用仓库外同名目录覆盖构建输入。

Windows 产品必须包含 GUI/core 的 token + daemon UUID 协议；`NKR_NO_GRPC`/`NKR_NO_EXTERNAL` 不再是可运行的产品构建，CMake 会明确拒绝，避免生成无法完成 ready 握手却表面可启动的包。

当前 lifecycle protocol version 为 `3`。GUI 为有界 RPC 发送标准 `grpc-timeout`，服务端 deadline 比本地 abort 提前 250 ms；Go lifecycle executor 可取消仍在等待准入的命令，Start candidate 的取消与发布由单一原子边界仲裁。已准入 Stop/Close 仍不可安全中断。Exit 不返回空响应：core 只在精确 `STOPPED` 时提交 `EXITING` 并返回结构化 lifecycle ACK，随后通过 one-shot gRPC `GracefulStop` 正常结束；active/blocked 不隐式 Stop。v1/v2 与 v3 的 GUI/core 混用会在 `GetDaemonInfo` 握手 fail closed，不能兼容回退。

## RPC schema 生成

唯一 schema 源是 `go/grpc_server/gen/libcore.proto`。准备过本仓库 Windows 工具链后运行：

```powershell
.\go\grpc_server\gen\update_proto.ps1
```

脚本优先发现仓内 `protoc 3.21.4`，并临时加入项目 MinGW 运行库；也允许用 `-Protoc` 显式指定同版本编译器。它把 `protoc-gen-go v1.28.1` 与 `protoc-gen-go-grpc v1.2.0` 固定安装到带独占锁的临时工具目录，只更新已跟踪的两个 Go 生成文件。首次缺少插件时需要联网执行固定版本的 `go install`，因此目前是版本固定但并非完全离线/密封的生成流程；不得并发执行。C++ `libcore.pb.h/.cc` 由 CMake 生成到 build 目录，不提交；旧的未固定 Unix `update_proto.sh` 已删除。重复运行后必须得到相同文件 hash。

## 必须验证

完整无 Skip package 后使用本轮 deployment 中的 wrapper；CMake GUI build tree 不输出或保留 `nekobox_core.exe`：

```powershell
.\deployment\windows64\nekobox_core.exe version
.\deployment\windows64\nekobox_core.exe check -c <config.json>
```

`check -c` 只证明 schema 和 pre-start 校验通过，不证明 Mixed 监听、DNS、TLS、AnyTLS 会话或真实连通。

Exit integration executable 不属于通用 core CLI，也不注册到 CTest；它只接受完整 package 脚本提供的路径/hash/work-root 授权。harness 使用无 listener、无 TUN 配置验证协议 v3 握手/deadline/ACK 与同一 QProcess `NormalExit/0`，不经过产品 Client/MainWindow，也不证明 Clash TUN、路由、DNS、WFP 或系统代理所有权。

截至 2026-07-22，当前源码已完成一次不带 Skip 参数、先 clean reset GUI build tree 的本地完整打包，tracker/share/resolver policy/raw Exit gate 均通过；core 只输出到 `deployment/windows64/nekobox_core.exe`，SHA-256 为 `F545DC44627B83DAF49786F3403ED9E464783D71E6917CE06FDFFC0E147D09E5`。`build-package-windows64/nekobox_core.exe` 不存在，也不作为 provenance 输入。package RouteFluent manifest SHA-256 为 `28100CC9F77DE340A3B76A873E476B8EA9D4ECB115B1BA347FFF57345184760A`。该 deployment 仍是被忽略的本地审计产物，不是 release manifest 或 Windows 集成验收；clean reset 也不等于独立 clean-room 工具链。`run/check` 会直接读取 sing-box 配置，安全边界见 [CLI 文档](../reference/CLI.md#core-高级-cli)。

RouteFluent 自有测试和 fixtures 应进入 CI；主项目仍需单独覆盖 GUI、数据库、导入和运行状态。
