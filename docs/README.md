# 文档索引

现行文档只描述本 Windows 私人分支。`archive/` 保存被推翻的 agent 设计、一次性证据和历史契约，不能作为当前需求。

## 先读

- [产品契约](PRODUCT.md)：唯一产品范围源。
- [接管状态](TAKEOVER_STATUS.md)：当前实现、证据和发布判断。
- [已知问题](KNOWN_ISSUES.md)：按 P0/P1/P2 排序的缺口。
- [推进路线](ROADMAP.md)：恢复上游能力、三项核心扩展与明确追加需求的实施顺序。
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

ADR 0005（MultiMapper）已撤出产品决策序列；相关材料只保留在历史归档。

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
- [fail-closed 验证](testing/FAIL_CLOSED.md)

## 历史归档

- [归档规则](archive/README.md)
- [2026-07-20 范围偏离审计](archive/audits/2026-07-20-scope-deviation-audit.md)
- [2026-07-20 接管运行证据](archive/audits/2026-07-20-takeover-baseline.md)
