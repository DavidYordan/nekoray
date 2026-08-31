# 文档索引

现行文档描述本私人分支。发生冲突时以用户最新要求和 [产品方向与开发契约](PRODUCT.md) 为准；实现、测试、ADR 和历史路线都不能反向创造需求。`archive/` 保存被推翻的 agent 设计、一次性证据和历史契约，不能作为当前需求。

每类事实只保留一个主要落点，避免再次出现“旧计划反向定义产品”的问题：

环境硬约束：当前开发主机依赖不可中断的 Clash TUN；任何开发或测试都不得停止、重启或改写它。WSL/OpenWrt 只能提供 Linux/core 层证据，Windows TUN/Wintun/系统代理/驱动验收必须转移到独立 Windows 隔离环境。具体规则见[产品契约](PRODUCT.md#34-本机基础网络与-tun-验证边界)与[Clash TUN 共存说明](operations/CLASH_TUN_COEXISTENCE.md)。

| 文档 | 唯一职责 | 不应承载 |
|---|---|---|
| [产品契约](PRODUCT.md) | 产品范围、术语和硬约束 | 当前完成度、临时修法 |
| [接管状态](TAKEOVER_STATUS.md) | 指定 commit/日期下的实现快照和发布判断 | 新需求、长期任务细节 |
| [已知问题](KNOWN_ISSUES.md) | 当前整改队列、优先级、完成门和阻止项审计 | 历史执行流水账 |
| [测试矩阵](testing/TEST_MATRIX.md) | 分层证据及其证明边界 | 用低层测试宣布产品完成 |
| [待确认决策](DECISIONS_NEEDED.md) | 确实需要用户决定的体验问题 | 可由上游或产品契约直接推出的工程问题 |
| [历史路线](ROADMAP.md) 与 `archive/` | 旧阶段记录和被取代方案 | 现行开发顺序 |

## 先读

- [Agent 工作规则](../AGENTS.md)：后续 agent 的项目级调查、开发、验证、文档与 Git 纪律。
- [产品方向与开发契约](PRODUCT.md)：唯一产品范围源；先读术语、三项核心能力、当前改动分类和开发纪律。
- [接管状态](TAKEOVER_STATUS.md)：当前实现、证据和发布判断。
- [已知问题](KNOWN_ISSUES.md)：现行、按依赖排序的整改安排；后续工作从这里选取最小可验证切片。
- [历史路线](ROADMAP.md)：2026-07-24 以前的执行台账；不得按其中复选框继续开发。
- [待确认决策](DECISIONS_NEEDED.md)：只保留无法从代码与现有要求推导的问题。

## 架构与决策

- [架构总览](architecture/OVERVIEW.md)
- [ADR 0001：Windows 私人项目](architecture/decisions/0001-windows-private-scope.md)
- [ADR 0002：最小化 NekoRay 分支与核心边界](architecture/decisions/0002-minimal-fork-boundary.md)
- [ADR 0003：订阅导入安全语义](architecture/decisions/0003-subscription-import-policy.md)
- [ADR 0004：Windows 运行时安全不变量](architecture/decisions/0004-runtime-safety-policy.md)
- [ADR 0006：Mixed 端口历史迁移（已被 0012 取代）](architecture/decisions/0006-mixed-port-migration.md)
- [ADR 0007：Mixed 端口到线路映射](architecture/decisions/0007-mixed-routing-contract.md)
- [ADR 0008：持久 Windows Runtime 与无直连切换（候选架构）](architecture/decisions/0008-persistent-windows-runtime.md)
- [ADR 0009：配置事务与显式恢复方向](architecture/decisions/0009-config-transaction-recovery.md)
- [ADR 0010：进程内生命周期串行化与 generation fencing](architecture/decisions/0010-process-local-lifecycle-generation-fencing.md)
- [ADR 0011：daemon 实例身份、生命周期对账与 Exit ACK](architecture/decisions/0011-daemon-identity-and-lifecycle-reconciliation.md)
- [ADR 0012：恢复 NekoRay 原生 Mixed 默认端口](architecture/decisions/0012-restore-native-mixed-port.md)

MultiMapper 不作为产品依赖或可直接移植的实现，但其多入口来源、候选、固定入口与分层诊断思想是本轮明确要求的参考材料。所有 ADR 均从属于产品契约；特别是 persistent Runtime/WFP 等候选架构不能作为三项核心需求的发布前提。

## 开发、操作与测试

- [Windows 构建](development/BUILD_WINDOWS.md)
- [core 构建](development/CORE_BUILD.md)
- [开发工作流](development/WORKFLOW.md)
- [CLI 与内部参数](reference/CLI.md)
- [备份与恢复](operations/BACKUP_AND_RECOVERY.md)
- [Mixed 排障](operations/TROUBLESHOOT_MIXED.md)
- [Clash TUN 共存与本机诊断](operations/CLASH_TUN_COEXISTENCE.md)
- [协议支持证据](reference/PROTOCOL_SUPPORT.md)
- [订阅导入规则](reference/SUBSCRIPTION_IMPORT.md)
- [测试矩阵](testing/TEST_MATRIX.md)
- [Core 配置导出](testing/CORE_CONFIG_EXPORT.md)
- [Windows 运行时连通性](testing/RUNTIME_CONNECTIVITY.md)
- [OpenWrt 隔离实验室](testing/OPENWRT_REMOTE_LAB.md)
- [全机 fail-closed 历史扩展验证（非现行发布门）](testing/FAIL_CLOSED.md)

## 历史归档

- [归档规则](archive/README.md)
- [2026-07-20 范围偏离审计](archive/audits/2026-07-20-scope-deviation-audit.md)
- [2026-07-20 接管运行证据](archive/audits/2026-07-20-takeover-baseline.md)
