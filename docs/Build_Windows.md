# Windows 构建

本项目的 Windows 正式构建入口是仓库根目录的：

```powershell
.\build_windows_package.ps1
```

不要再使用上游旧文档中的手工流程，例如手工把 Qt/MinGW bin 加入 PATH、手工运行 `windeployqt`、手工复制核心文件。这些动作必须由脚本统一完成。

## 构建产物

脚本完成后会生成：

- 部署目录：`deployment/windows64`
- 正式 zip：`deployment/nekoray-<version>-windows64.zip`

`deployment/windows64` 用于本机测试，会在构建后恢复本机用户配置。

正式 zip 默认不携带本机 `config` 目录，避免把测试订阅、账号、分组等私人数据放入发布包。

## 配置保留规则

脚本会自动执行以下步骤：

1. 只查找并关闭 `deployment/windows64` 下正在运行的本项目进程。
2. 不触碰 `D:\Program Files\nekoray` 等生产安装目录。
3. 备份 `deployment/windows64/config`。
4. 删除并重建 `deployment/windows64`。
5. 构建 GUI、RouteFluent patched sing-box、`nekobox_core.exe`、`updater.exe`。
6. 生成 zip。
7. 把备份的 `config` 恢复回 `deployment/windows64/config`。

如果构建中途失败，脚本也会尽量恢复已备份的配置。

## 本地依赖

依赖应尽量保持在项目目录内：

- Qt：`qtsdk/qt/...`
- MinGW：`qtsdk/tools/Tools/...`
- C/C++ 依赖：`libs/deps/built`
- RouteFluent patched sing-box：`third_party/routefluent-sing-box`
- 临时工具：`tools`

缺少依赖时可以下载到本项目目录，不应污染全局环境。

## 常用参数

```powershell
.\build_windows_package.ps1
.\build_windows_package.ps1 -SkipGoBuild
.\build_windows_package.ps1 -SkipGuiBuild
.\build_windows_package.ps1 -RefreshGeodata
```

通常使用无参数构建即可。
