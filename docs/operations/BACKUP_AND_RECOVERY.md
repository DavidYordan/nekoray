# 备份与恢复

状态：现行
最后更新：2026-07-22

## 风险说明

接管工作树已让普通单文件保存和已接入的多文件操作共享参与 mutation 路径的提交串行化 mutex 与跨进程磁盘锁；这不是覆盖所有模型读取和写入的完整读写锁。每次实际改变内容的单文件保存都会在 `QSaveFile::commit()` 前发布可恢复的 durable before/after intent，并继续使用禁止 direct-write fallback 的原子替换和内容寻址备份；已接入的多文件操作则先持久化、校验 before/after manifest。未知/损坏 profile 不再自动删除，危险 `-flag_reorder` 也已禁止执行。损坏/未知数据和已识别的悬空引用会保留原件并写入隔离证据；显式删除单个 profile 或空 group 前也必须先建立可验证快照。已有显式命令行事务恢复入口，但恢复 GUI、未知对象自动修复、订阅成功候选和非空 group 事务仍未完成，因此任何真实数据操作前仍应先备份完整配置目录。

## 自动恢复证据

运行时恢复目录位于当前配置根目录的 `recovery/`：

- `recovery/backups/<原相对路径>.<旧内容 SHA256>.bak`：覆盖已有配置前的原文副本。写入后会回读逐字节验证；无法建立或验证时，目标配置不会被覆盖。
- `recovery/quarantine/<原相对路径>.<内容 SHA256>.snapshot`：损坏、未知、ID 不一致或悬空引用文件的原文快照。
- `recovery/deletions/<原相对路径>.<内容 SHA256>.snapshot`：准备执行显式删除时建立的原文快照。它表示“删除已准备”，不保证之后的多文件操作和源文件删除已经成功。
- `recovery/quarantine/` 与 `recovery/deletions/` 中同名 `.meta.json`：记录对应 schema、原相对路径、SHA256、大小、首次发现/准备时间和一个或多个原因。
- `recovery/transactions/<UUID>/manifest.json`：单文件 intent 或多文件事务的操作名、状态、目标相对路径及 before/after SHA256。相邻 `before/`、`after/` 保存逐字节证据；只有清单以原子方式从暂存目录发布后，目标文件才可能开始变化。多文件终态目录作为历史证据持续保留。
- `recovery/retired-single-file-transactions/<UUID>/`：已精确验证为 before（`VerifiedBefore`，manifest 终态为 `aborted`）或 after（`VerifiedAfter`，终态为 `committed`）的单文件 intent 会先原子移入这里，再尽力递归删除；正常情况下它只是短生命周期过渡目录。无法判断目标状态或无法写入终态时按 `Indeterminate` 保留原 `prepared` intent，并阻断后续 mutation/启动。

保存还会在写入前和 commit 前比较磁盘内容与本进程最初加载的字节；若文件被人工或其它进程修改，本次保存失败，不会用旧内存状态覆盖新磁盘内容。相同内容只对应同一路径，已有备份被篡改时后续保存会被阻断。

GUI 发现 profile/group 层问题时只显示恢复提示，不自动修改原件。主配置或活动路由本身损坏时仍会中止启动，并在 stderr/日志中给出原因。`recovery/` 可能包含订阅、节点和凭据，应当与配置目录同级保护，禁止提交 Git 或公开上传。

除上述已验证的单文件终态 intent 会自动退役并尽力删除外，当前没有通用保留期限或自动清理策略，以避免再次出现静默删证据。内容寻址备份、隔离/删除快照、多文件终态事务，以及退役删除失败留下的目录都可能长期增长。清理或恢复必须在维护窗口中人工核对 snapshot、metadata 和 SHA256。不要把 `.bak` 直接覆盖回运行中的配置。

## 删除保护

- 删除单个 profile 或空 group 前，源文件必须仍与本进程加载的字节完全一致，并且 `recovery/deletions/` 快照与元数据必须原子写入、回读验证成功；任一步失败都会保留源文件和已加载对象。
- 普通 `JsonStore::Save()`、group 创建、单 profile/空 group 删除、profile 跨组移动和非活动 route 删除均使用同一进程内提交串行化 mutex 与跨进程 `active.lock`。每个有内容变化的单文件保存先发布短生命周期 durable before/after intent；其余已接入的多文件操作使用持续保留的 before/after manifest。group 创建会把 `groups/<id>.json` 与 `groups/pm.json` 一次提交，不再留下只存在 group 文件而 manager 顺序未持久化的窗口。
- 启动扫描会枚举事务根下普通、隐藏和系统条目，并对活动锁、意外文件/目录、缺失或无法解析的 manifest、schema/id 不匹配和任何非终态 state 失败关闭；只有协议生成且尚未发布的精确 `.staging-<小写 UUID>` 可忽略。`committed`、`rolled_back`、`aborted` 在启动路径只校验 JSON 与 schema/id/state header 后放行，不会深解析 operation、entries 或 snapshot；维护 report 才会深解析终态证据。合法 terminal header 即使 entries 损坏或为空也不阻断 startup，但 report 必须标为 `valid=false`；非法 terminal schema 仍阻断。因此终态目录存在不等于其全部证据已由启动扫描审计。
- 普通中途失败会按相反顺序恢复已经写入的目标。若回滚或状态标记本身失败，当前进程会禁止继续保存配置；下次启动会在加载任何配置前停止，并报告事务目录。它绝不会自动选择提交或回滚。
- 被 group front proxy 或其它 chain 引用的 profile 会被拒绝删除，必须先由用户显式解除引用。
- 非活动 route 也通过事务层删除；route 名必须是安全的 Windows 单文件名。活动 route 不允许直接删除，必须先由用户显式加载并确认另一个 route。
- 非空 group 的级联删除当前明确禁用。旧实现会忽略子 profile 删除失败并继续移除 group，可能形成半删除；在真正的多文件事务完成前，用户只能先逐项移动或删除 profile，再删除空 group。
- 删除后的 profile/group 会先设为不可保存再移出 manager，并保留删除前存在性作为 CAS 二次防线；仍持有旧 `shared_ptr` 的测速线程或窗口不能把已删除文件重新创建。core 运行时拒绝删除当前 auxiliary。订阅在联网前会按值快照不可变 HTTP 选项，并记录目标 group 及全部成员的身份、顺序、tombstone 与序列化状态；提交回 UI 线程并取得提交串行化 mutex 后会逐项重验。不过成功候选仍由多次 profile/group 保存和删除组成，尚未合并为一笔文件系统事务。
- 订阅清理或回滚若无法删除某个对象，会保留对象并在日志中明确报告，不再把它计作无条件成功。
- 删除前快照可能在后续步骤失败时仍然存在，这是刻意保留的审计证据。已接入操作会把相关 profile、group order 与主配置纳入同一 manifest；订阅成功候选、非空 group 级联和其它尚未迁移的调用仍可能跨多个独立提交，不能把当前保护误称为全局完整事务。

## 显式事务恢复

若启动报告未完成事务，不要删除 `recovery/transactions/<UUID>`，也不要手工拼接唯一真实配置。先停止该部署目录的实例并复制完整 `config/`，然后在同一二进制上生成审计报告：

```powershell
.\nekobox.exe -appdata <目标数据目录> -flag_config_transaction_report
```

维护命令要求恰好一个 `-appdata [目录]` 根选择器。禁止把它直接指向 `D:\Program Files\nekoray`、生产配置或其任何别名；应先把整个待分析配置复制到隔离根，再让命令作用于该副本。选定的配置根本身是信任锚，代码只拒绝其下级路径中的 reparse/junction，因此操作者还必须独立确认 `-appdata` 根和其 `config/` 不是 junction、符号链接或指向生产目录的别名。单独写 `-appdata` 表示 Windows 默认应用配置目录，不适合此维护流程；显式目录必须与待恢复副本完全一致。

报告不会加载 profile/group、修改已有用户配置、启动 core 或访问网络，但首次运行可能创建 `appdata/`、`config/`、`recovery/transactions/` 和锁文件并短暂持有 `active.lock`，所以它不是对所选根完全无写入的命令。它会核对 manifest 身份、snapshot 路径/大小/SHA256、Windows 大小写别名、配置根以下的 reparse/junction，以及每个目标当前是否精确等于 `before` 或 `after`。只有 `recoverable=true` 才允许继续。

人工确认方向后，明确选择其一：

```powershell
# 放弃候选操作，恢复所有 durable before 状态
.\nekobox.exe -appdata <目标数据目录> -flag_config_transaction_recover <UUID> before

# 完成候选操作，应用所有 durable after 状态
.\nekobox.exe -appdata <目标数据目录> -flag_config_transaction_recover <UUID> after
```

恢复开始前先把方向写入 manifest；若恢复中断或失败，后续只能重试同一方向，不能翻转。全部目标重新读取并验证完成后才写为 `rolled_back` 或 `committed`。任一目标既不匹配 before 也不匹配 after、snapshot 被篡改、方向元数据矛盾、清单损坏或锁被其它进程占用时都会零猜测拒绝；有效锁不会仅因超过 30 秒被另一进程按年龄抢占。这套 CLI 只恢复结构正确、证据完整的事务，不会自动修复 unknown/quarantine profile、损坏模型或悬空引用。这些情况仍需在配置副本上人工分析。当前没有图形恢复 UI，也没有自动选择方向。

## 配置位置

- 便携模式：`<package-dir>/config/`
- `-appdata <dir>`：`<dir>/config/`
- `-appdata` 未指定目录：Windows 应用配置目录下的 `config/`

不要只备份 `profiles/`；group order、路由、缓存和全局设置也会影响恢复。

## 安全备份

1. 只在预先安排的安全维护窗口处理。若内部 TUN 已开启，当前版本会阻止停线/退出且尚无持久保护层；不要为了备份强退或自行关闭 TUN，应另约隔离环境或维护窗口。
2. 在不依赖本项目系统代理/TUN 的隔离状态下，由用户显式停止本项目线路；Windows 系统代理当前只能在系统设置中人工核对，产品内切换已暂禁。
3. 确认没有来自该部署目录的 `nekobox.exe`/`nekobox_core.exe` 正在写配置。
4. 复制整个 `config/` 到带时间戳的独立目录。
5. 记录源目录、当前 commit 和二进制 SHA256。
6. 在副本上确认 `groups/*.json`、`profiles/*.json` 可以解析。

不要因为另一安装目录仍有同名进程就批量结束所有 core。

## 恢复

1. 停止目标部署目录中的本项目实例。
2. 保留当前损坏目录，改名为故障快照，不要直接覆盖唯一证据。
3. 把完整备份恢复为 `config/`。
4. 当前开发版会保留但不会展示尚未恢复的 Naive、旧外置 core 或未知类型；只在副本上验证，不能把“未显示”当成可删除。

## 订阅特别规则

当前刷新已先在内存解析/暂存；HTTP 请求使用联网前按值取得的不可变选项，HTML、坏 YAML、无效结构和零支持节点会保持旧组不变。联网完成后还会在 UI 提交阶段重验目标 group 以及全部成员的身份、顺序、tombstone 和完整序列化快照。但成功候选的多 profile/group 写入仍不是单个文件系统事务，崩溃或磁盘故障仍可能留下新增或残留节点。在完整事务和故障注入完成前：

- 只在完整备份后测试手动刷新；
- 不在唯一真实数据上启用无人值守自动刷新；
- 不使用 `sub_clear`；
- 保存 group 和 profile 的完整备份。
