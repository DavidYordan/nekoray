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

诊断时可使用 `-SkipGoBuild` 或 `-SkipGuiBuild`，但正式产物禁止使用跳过参数，因为它们可能把不同提交的旧二进制混入同一个 zip。

`-ReferenceDir` 默认留空，只允许用户显式提供一个经过核对的 geodata 缓存目录；它与其它构建输入使用同一套共享路径护栏。路径规范化后必须解析为本地固定磁盘的盘符路径；UNC/设备命名空间、网络映射或可移动盘、SUBST/DOS 设备重定向盘符、含 `~` 的 8.3 短路径以及任一已存在的 ReparsePoint/junction 组件均会在使用前拒绝。如果另一本地盘符指向与 `D:` 相同的物理卷，护栏会按该卷上的同一相对路径再次比对受保护生产根。也不得把任何其它安装目录的 core、配置或进程当作构建输入。

完整打包不停止任何项目进程。共享路径护栏会拒绝把受保护的 `D:\Program Files\nekoray` 或其子路径作为 build/toolchain 输入；其 `2080` 和 TUN 永远不在构建或清理范围内。这是保守的路径前置筛选，不是完整的 Windows 文件身份沙箱：它尚未通过最终文件句柄核对 final-file identity，因此不能识别所有已存在 hardlink 等同文件别名。构建仍应使用专用、可核对的非生产根。

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
.\build-package-windows64\nekobox_core.exe version
```

直接运行增量 CMake 或 CTest 时必须让 MinGW `bin` 位于 `PATH`。否则 `cc1plus.exe` 或测试程序可能因找不到运行库以 `0xC0000135` 退出；Ninja 可能只显示无编译诊断的 `code=1`，CTest 在 Windows 错误对话框被抑制前也可能表现为超时。这不是源码编译或测试逻辑错误。完整打包脚本会设置该环境。

CTest 当前包含 2 项。`config_recovery_test` 验证备份、隔离证据、删除前快照、事务提交/模拟回滚/锁/pending、before/after 恢复与方向锁、单文件 intent、终态退役和目录身份边界；`runtime_transition_test` 验证 process-local transition、depth gate、crash-cleanup handoff，以及 daemon/profile-request generation 的顺序和并发状态行为。它们不启动真实 GUI/core，也不代表订阅、非空 group、完整 ConfigBuilder、Windows TUN/WFP 或系统代理已经自动验收。发布前还必须完成 [测试矩阵](../testing/TEST_MATRIX.md)。

截至 2026-07-22，接管工作只重建并验证了 `build-package-windows64/` 中的 GUI/core；`deployment/windows64/` 仍是 2026-07-18 的旧产物。只有不带 `-SkipGoBuild`/`-SkipGuiBuild` 的完整打包成功，且 deployment/zip 的版本、hash 与 manifest 复核一致后，才能把 deployment 当作候选交付物；当前两处产物不得混用。

## 发布限制

当前禁止正式发布，直到完成：

- 单一版本源并写入 GUI、PE、core、zip 和 tag；
- 干净 Windows 环境构建；
- 二进制和依赖 manifest、SHA256、许可证与 SBOM；
- 禁止复用其他提交的旧二进制；
- 数据安全和运行时 P0 全部关闭。
