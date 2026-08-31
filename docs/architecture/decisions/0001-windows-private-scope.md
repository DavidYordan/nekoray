# ADR 0001：Windows 私人项目

状态：Accepted
日期：2026-07-20

## 决策

主项目仅面向 Windows，并且只供私人使用。

## 影响

- 删除现行 Linux 构建和运行承诺。
- 不维护 AUR、Scoop、公共 Release、Telegram、捐赠或公开支持渠道。
- 首期 CI、构建、测试和发布资源全部投入 Windows。
- 第三方子模块自己的跨平台文档不受此决策影响。

## 不在本 ADR 中的决定

- Windows 10/11/Server 的最低版本。
- x64 是否是唯一架构，还是未来需要 ARM64。
- 便携目录或安装器/Program Files 模式。
- 中文作为当前主文档语言，但是否是唯一维护语言尚未确认。
