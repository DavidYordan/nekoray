# 开发工作流

## 分支与提交

- 不再直接在 `main` 上连续堆叠功能。
- 每个变更使用独立分支，范围限定为一个问题或一个 ADR。
- 提交正文记录需求、风险、验证命令和残余限制。
- P0 修复先写失败回归，再修改实现。

## 变更顺序

1. 更新或引用现行需求/ADR。
2. 增加能够复现问题的测试或 fixture。
3. 实现最小修复。
4. 运行与风险相称的验证。
5. 更新 `KNOWN_ISSUES.md` 和测试矩阵。
6. 经审查后合入。

## 数据和运行安全

- 真实 `config/`、订阅、core JSON、日志和审计报告不得提交。
- 本机 Clash TUN 是外部基础网络和永久 no-touch 资源。任何 agent、脚本、构建和测试都不得擅自停止、写入或接管它；临时绕过只允许作用于隔离诊断副本或进程，并必须显式记录。
- 完整打包若发现精确目标 deployment 中仍有运行进程必须 fail-fast；脚本不得停止或强杀它。只能由用户明确的 GUI/运维操作停止该目标实例后重试，禁止按进程名或端口扩大目标。
- 本地测试默认不得修改系统代理、TUN、WFP、默认路由或 DNS。Windows 专属故障注入只有在文档列明精确动作、恢复证据并由用户安排维护窗口后才能执行。
- 诊断进程必须按确切 PID 回收。
- 删除和迁移数据前必须建立可验证备份。

## 双环境验证

1. 本地 fixture、配置导出、schema 和纯逻辑测试始终在本地隔离目录执行。
2. Clash TUN 使真实 outbound 归因不清时，先用进程级临时对照；仍不清楚时按 [OpenWrt 隔离实验室](../testing/OPENWRT_REMOTE_LAB.md) 使用 `192.168.1.7` 的临时 loopback probe。
3. 远端 probe 只允许精确临时目录、固定测试端口和精确 PID；不得修改 RouteFluent 现有 service、配置、nftables、路由或监听。
4. OpenWrt 只证明 sing-box 配置/协议/outbound，不替代 Windows Wintun、系统代理、WFP、GUI 或重启验收。
5. 只有 Windows 专属行为确实无法在 Clash TUN 保持运行时验证，才向用户提出维护窗口；程序不得自行关闭 Clash TUN。

## 文档规则

- 现行规范放在产品、架构、reference 或 testing 目录。
- 一次性调查放 `archive/audits/`，过程计划放 `archive/plans/`。
- 旧契约必须标明 superseded，不与现行契约同时出现在主索引。
- “已实现”“构建通过”“schema 通过”“真实连通”“已验收”不得混用。
