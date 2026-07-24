# ADR 0009：配置事务与显式恢复方向

状态：已接受
日期：2026-07-22

## 背景

NekoRay 的 profile、group、`groups/pm.json`、主配置和 route 是独立文件。旧实现按顺序调用多个 `Save()`/删除操作；写入失败、进程中断或并发刷新会留下半提交状态。自动猜测“应该保留新值还是旧值”会进一步覆盖唯一证据。

## 决策

1. 普通单文件保存和已迁移的多文件操作共享进程内的**提交串行化 mutex**与跨进程 `recovery/transactions/active.lock`。该 mutex 只覆盖已接入的 mutation/提交路径，不是整个 ProfileManager、配置生成器或 UI 的完整模型读写锁，也不保证长操作持有不可变输入快照。
2. 每次实际改变内容的单文件 `JsonStore::Save()` 都在 `QSaveFile::commit()` 前发布短生命周期、可恢复的 before/after intent。目标仍精确处于 before 时可以标记 `aborted`，写后验证为 after 时可以标记 `committed`；两种已确认终态随后移入 `recovery/retired-single-file-transactions/` 并尽力删除。无法确认目标或无法写终态时保留 `prepared` intent 并阻断后续 mutation/启动。
3. 在任何已接入的多文件目标变化前，事务必须发布 `nekoray.config_transaction.v1` manifest，并保存、回读校验每个存在状态的 before/after snapshot。多文件终态 manifest 作为审计证据持久保留。
4. 启动扫描对活动锁、意外/隐藏根条目、manifest 缺失或无法解析、schema/id 不匹配及任何非终态 state 失败关闭；协议生成且尚未发布的精确 `.staging-<小写 UUID>` 可忽略，因为此阶段目标尚未变化。对 `committed`、`rolled_back`、`aborted` 只校验 JSON 与 schema/id/state header 后放行；维护报告会再深解析 operation、entries、snapshot 和目标状态。合法 terminal header 即使 entries 损坏或为空也不阻断启动，但报告必须标记 `valid=false`；非法 terminal schema 仍阻断。因此不得把启动扫描描述成对全部终态证据的完整审计。
5. 显式恢复只能由维护命令选择完整 `before` 或完整 `after`。执行前必须证明每个当前目标精确匹配两者之一，且 snapshot 的路径、大小和 SHA256 均可信。
6. 恢复方向先写入 manifest。进入 `recovering_before`、`recovering_after` 或 `recovery_failed` 后，只允许重试同一方向；全部目标验证完成后才标记 `rolled_back` 或 `committed`。
7. manifest/snapshot 损坏、方向元数据矛盾、目标发生第三种变化或存在活动写锁时拒绝恢复，不提供启发式修补。Windows 路径按大小写不敏感处理；配置根以下的组件拒绝尾随点/空格、短名歧义字符 `~`、保留设备名及 reparse/junction。用户选定的配置根本身是信任锚，必须由操作者确认不是 junction/别名；全部目标在写终态前重新读取验证。
8. 删除对象在提交后 tombstone，防止仍持有旧对象的异步任务重新创建文件。
9. `active.lock` 禁止按 30 秒年龄抢占；启动检查会持续持锁到完整配置加载完成，关闭“检查后、加载前”被另一实例发布事务的窗口。

## 后果

- 崩溃留下的是可审计、可选择方向的阻断状态，不是静默半提交。
- 单文件保存会短暂产生 durable intent；正常 verified-before/after 终态会自动退休，无法确认的 intent 必须保留供显式恢复。内容寻址备份、隔离/删除证据和多文件终态事务仍会持续增长，在保留策略完成前禁止自动清理。
- 命令行恢复不加载 profile/group、不启动 core、不访问网络，可在 GUI 尚未实现前用于受控维护；它只恢复结构正确的事务，不自动修复 unknown/quarantine 模型。
- 订阅成功候选和非空 group 级联尚未迁移；配置生成也未取得跨全部读取的 immutable snapshot。不能据此宣称整个配置模型已事务化或已具备完整读写同步。

操作命令与维护步骤见[备份与恢复](../../operations/BACKUP_AND_RECOVERY.md#显式事务恢复)。
