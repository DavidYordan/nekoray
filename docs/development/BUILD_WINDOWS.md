# Windows 构建

状态：现行；尚无可发布产物
最后更新：2026-07-22

## 正式入口

在仓库根目录运行：

```powershell
.\build_windows_package.ps1
```

产物：

- 本机部署目录：`deployment/windows64/`
- zip：`deployment/nekoray-<version>-windows64.zip`

脚本会备份本机部署目录下的 `config/`、重建部署目录，然后恢复配置。若 `deployment/windows64` 中仍有 GUI/core/updater 运行，脚本会 **fail-fast** 并列出精确 PID/路径；它绝不会自动关闭或强杀实例。请只通过用户明确的 GUI/运维动作处理目标实例后再重试。

## 当前依赖

- Qt 6 和匹配的 MinGW：默认从 `qtsdk/` 自动选择。
- C/C++ 依赖：`libs/deps/built/`。
- RouteFluent wrapper：`third_party/routefluent-sing-box/`。
- 固定 libneko：`third_party/libneko/` 子模块。
- Go、Python、CMake、Ninja 和 PowerShell。

Windows 产品构建强制启用已鉴权的 GUI/core RPC；`NKR_NO_GRPC`（以及隐式设置它的 `NKR_NO_EXTERNAL`）会在 CMake 配置期明确失败，不能作为精简产品变体。

Qt/MinGW 和 C/C++ 预构建依赖尚未全部由仓库自动获取，因此当前构建仍不是完整可复现的干净构建；Go 的 libneko 输入已经锁定在仓内子模块。

## 持续集成边界

`.github/workflows/windows-quality.yml` 在 `windows-2022` 上执行仓库卫生检查、固定子模块检出、受控 RouteFluent core 源构建、两个 Go 模块普通测试和无侵入 Python 安全契约。该门禁故意不读取本机生产安装，也不启用 TUN 或系统代理。

当前 CI 尚未取得完整 Qt/MinGW/C++ 依赖，因此不构建 GUI；通过该工作流只证明其列出的静态与 Go 检查，不代表 GUI、Windows TUN/WFP、系统代理 broker、切线或退出语义通过。

## 参数

```powershell
.\build_windows_package.ps1 `
  -BuildDir build-package-windows64 `
  -QtDir <qt-dir> `
  -MingwDir <mingw-dir> `
  -DepsDir <deps-dir> `
  -Parallel 4
```

诊断时可使用 `-SkipGoBuild` 或 `-SkipGuiBuild`，但它们可能在 package 目录中混入不同提交的旧二进制，因此只允许诊断：脚本会跳过真实 core Exit gate，并且不会创建或覆盖正式 zip。正式 archive 必须使用无 Skip 流程。

`-ReferenceDir` 默认留空，只允许用户显式提供一个经过核对的 geodata 缓存目录；它与其它构建输入使用同一套共享路径护栏。路径规范化后必须解析为本地固定磁盘的盘符路径；UNC/设备命名空间、ADS/额外冒号命名空间、网络映射或可移动盘、SUBST/DOS 设备重定向盘符、含 `~` 的 8.3 短路径以及任一已存在的 ReparsePoint/junction 组件均会在使用前拒绝。如果另一本地盘符指向与 `D:` 相同的物理卷，护栏会按该卷上的同一相对路径再次比对受保护生产根。也不得把任何其它安装目录的 core、配置或进程当作构建输入。

完整打包不停止任何项目进程，也不停止或改写本机 Clash TUN。共享路径护栏仍拒绝把历史安装目录 `D:\Program Files\nekoray` 或其子路径作为 build/toolchain 输入；这是保守的路径前置筛选，不是当前端口所有权声明，也不是完整的 Windows 文件身份沙箱。构建应使用专用、可核对的工作根。

只要没有 `-SkipGuiBuild`，脚本就先由共享安全 helper 删除精确 build 子树并重新创建，再配置 CMake；helper 会拒绝仓库根、生产路径/别名和 reparse tree。正式无 Skip 流程因此不会继承旧 CMake cache、object、测试程序或手工诊断 core，并强制 `BUILD_TESTING=ON`。GUI 测试目标和 package core 都构建完成后，脚本依次运行 `runtime_transition_test`、`share_format_test` 和 `core_exit_integration_test`，全部通过后才进入正式 zip。tracker 测试覆盖 `{generation, UUID, PID}` finished 的精确身份、异常完成和 finished-before/after-wait；分享测试只用假凭据验证格式转换；integration harness 还要求 package 脚本提供精确 core 路径、SHA-256 与隔离 work root 授权，并再次拒绝生产路径、reparse/SUBST/hardlink 到生产 core。

integration harness 使用随机非 `2080`/`12080` loopback 控制端口、`NoProxy` raw Qt HTTP/2 client、随机 token/UUID 和无 listener/无 TUN 的最小配置。它验证 lifecycle protocol v3 握手、服务端 deadline header、错误 UUID、Exit non-admission 对账 fence及迟到命令拒绝、active Exit 拒绝、显式 Stop、结构化 `EXITING` ACK 和同一 test-owned QProcess `NormalExit/0`；只比较常见 WinINet 五键。测试失败时先尝试已鉴权 Stop/Exit，最后才允许 terminate/kill 它自己创建且 PID 仍精确匹配的无 listener/无 TUN 进程。该清理权限不属于产品代码，也绝不适用于生产 core。

配置/旧 Go 二进制备份具有单次构建 ownership。只有本次运行成功创建的备份才会在本次 `finally` 恢复；若发现 `deployment/windows64-config-preserve` 或 `deployment/windows64-binary-preserve` 已存在，脚本会在改动 package 前失败并保留现场，要求人工核对/恢复，绝不会删除或自动套用上次中断遗留的备份。

作为缩小 hardlink 覆盖风险的额外策略，`export_profile_core_config.ps1`、`verify_runtime_connectivity.ps1` 和 `verify_fail_closed_restart.ps1` 在单文件导出/报告目标已存在时拒绝覆盖。这只缩小一条已知覆盖路径，不能代替最终句柄身份校验。

## 最低验证

增量接管验证先针对构建目录中的同轮 GUI/core：

```powershell
$mingwRoot = Get-ChildItem .\qtsdk\tools\Tools -Directory -Filter 'mingw*_64' |
    Sort-Object Name -Descending |
    Select-Object -First 1
$env:Path = "$($mingwRoot.FullName)\bin;$env:Path"
cmake --build build-package-windows64 --target nekobox --parallel 2
ctest --test-dir build-package-windows64 --output-on-failure
.\deployment\windows64\nekobox_core.exe version
```

直接运行增量 CMake 或 CTest 时必须让 MinGW `bin` 位于 `PATH`。否则 `cc1plus.exe` 或测试程序可能因找不到运行库以 `0xC0000135` 退出；Ninja 可能只显示无编译诊断的 `code=1`，CTest 在 Windows 错误对话框被抑制前也可能表现为超时。这不是源码编译或测试逻辑错误。完整打包脚本会设置该环境。

CTest 当前包含 4 项。`config_recovery_test` 验证配置事务与目录身份边界；`runtime_transition_test` 验证 process-local transition、depth gate、crash-cleanup handoff、daemon/profile-request generation 和 finished tracker；`share_format_test` 验证无 fragment 原生链接及严格 `ip:port:user:pass` 转换；`resolver_policy_test` 验证 WD/NEX resolver 来源选择、DoH URL、domain/IP endpoint bootstrap 和 strict resolver group。四者都不启动真实 GUI/core，分享测试也不操作系统剪贴板。真实 core harness 故意不注册到 CTest，因为任意增量 CMake build 不能证明某个被忽略的 deployment core 来自当前源码；它只由完整无 Skip package 对刚构建的 package core 运行，不能直接作为日常 CTest 调用。

raw harness 不调用产品 `NekoGui_rpc::Client::Exit` 或 MainWindow continuation 流程，所以不是 GUI→Client→core 端到端门禁。其配置没有 listener/TUN，WinINet 五键快照也不证明生产 PID/`2080`、适配器、路由、DNS、TUN 或 WFP 不变。它不能把产品从 Alpha 升级为可发布；完整边界见[测试矩阵](../testing/TEST_MATRIX.md)。

截至 2026-07-22，当前源码已完成一次不带 `-SkipGoBuild`/`-SkipGuiBuild`、先 clean reset GUI build tree 的本地完整打包；tracker、分享格式、resolver policy 与 raw real-core Exit gate 均 PASS，zip 也已生成，215 个 package 配置文件已恢复，且无 preserve 或手工诊断产物遗留。`build-package-windows64/nekobox.exe` 与 `deployment/windows64/nekobox.exe` 的 SHA-256 都是 `3E918885EBB20D0A00FF04FD43E16841E5C0453CCD324C6F5EDE2BB3C3EBB43D`；core 只输出到 `deployment/windows64/nekobox_core.exe`，SHA-256 为 `F545DC44627B83DAF49786F3403ED9E464783D71E6917CE06FDFFC0E147D09E5`，clean GUI build tree 中不存在另一个 core。zip SHA-256 为 `86F3CD775DFF03B13FF6A66DC225FFA1BDDA0B919D504542384C0D743CFBC306`；package RouteFluent manifest SHA-256 为 `28100CC9F77DE340A3B76A873E476B8EA9D4ECB115B1BA347FFF57345184760A`。这些 deployment/zip 是被忽略的本地验收产物；独立干净工具链、版本化 release manifest 与 Windows 集成矩阵未完成前仍不得作为候选交付物。

## 发布限制

当前禁止正式发布，直到完成：

- 单一版本源并写入 GUI、PE、core、zip 和 tag；
- 干净 Windows 环境构建；
- 二进制和依赖 manifest、SHA256、许可证与 SBOM；
- 禁止复用其他提交的旧二进制；
- 数据安全和运行时 P0 全部关闭。
