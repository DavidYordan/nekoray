# NekoRay Agent 工作规则

本文件是本仓库内 Codex/Agent 的项目级执行规范。它约束后续调查、开发、测试、文档和 Git 行为，用于防止多轮迭代后再次出现范围扩张、上游回归、重复语义和无法审计的大规模整改。

本文件不是第二份产品需求。产品范围的唯一权威文件是 `docs/PRODUCT.md`。

## 最高优先级原则

1. **这是成熟 NekoRay 的最小补强，不是绿地重写。** 上游 NekoRay 4.0.1 `adef6cd` 的稳定行为默认保留；新增能力必须局部接入，不能借机重做整个客户端。
2. **需求权威唯一。** 每项产品改动必须能映射到 `docs/PRODUCT.md` 的具体条目。当前代码、测试、旧 ADR、历史路线、RouteFluent 或 MultiMapper 都不能反向创造需求。
3. **先证明必要，再扩大改动面。** 修复一个字段、协议、listener 或 UI 操作时，默认只改其最短数据链。没有复现和收益证据，不做顺手重构、全局抽象、批量重命名、全文件格式化或“统一架构”。
4. **手工选择优先于自动策略。** 诊断、延迟、GeoIP、单次失败和晚高峰现象只能形成证据，不能自动换入口、换线路、换 resolver、轮询、负载均衡或触发 fallback。
5. **失败关闭必须有明确作用域。** provider resolver 和专用并发端口按产品契约失败关闭；不得把这一要求扩张成全机 WFP、持久 Windows service、全新 runtime 或所有 NekoRay 流量的统一重写。
6. **保留兼容数据，拒绝静默猜测。** 未知、旧版、损坏或暂不支持的数据应保留、提示或隔离，不能删除、重排、复用 ID、按空值继续或静默改投其它能力。
7. **诊断不修改运行选择。** Resolve、Ping、URL Test、DNS/TCP/TLS/协议探测及审计工具默认只读；除非用户明确执行“应用/固定/解除”等动作，否则不得写 profile、入口或 OS 网络状态。
8. **完成结论必须有相称证据。** “代码存在”“构建成功”“schema 通过”“真实线路闭环”“Windows 集成通过”和“用户验收”必须分开陈述。

## 权威顺序与必读文档

产品语义冲突时按以下顺序处理：

1. 用户最新的明确指令；
2. `docs/PRODUCT.md`；
3. 当前代码、测试和可复现运行证据；
4. `docs/TAKEOVER_STATUS.md`、`docs/KNOWN_ISSUES.md`；
5. ADR、旧路线和归档材料。

`docs/ROADMAP.md` 是 2026-07-24 以前的历史执行台账，不是现行开发顺序。ADR 0004/0007/0008/0012 中已标记 superseded 的部分不得复活为产品要求。

每个任务开始前至少执行和阅读：

1. `git status --short --branch`，确认分支和已有脏改动；
2. 本文件；
3. `docs/PRODUCT.md`；
4. 与任务直接相关的代码、测试和现行文档；
5. 必要时对照上游基线 `adef6cd`，而不是只对照当前分支。

按任务类型补充阅读：

| 范围 | 先读 |
|---|---|
| 订阅、Clash YAML、resolver、多入口 | `docs/reference/SUBSCRIPTION_IMPORT.md`、`docs/PRODUCT.md` 第 4 节 |
| AnyTLS、Trojan、front proxy、detour | `docs/reference/PROTOCOL_SUPPORT.md`、`docs/development/CORE_BUILD.md`、`docs/PRODUCT.md` 第 5 节 |
| 主 Mixed、专用并发端口、路由 | `docs/testing/RUNTIME_CONNECTIVITY.md`、`docs/architecture/OVERVIEW.md`、`docs/PRODUCT.md` 第 6 节 |
| 保存、订阅刷新、恢复 | `docs/operations/BACKUP_AND_RECOVERY.md`、相关数据模型与现有测试 |
| 构建、打包、部署目录 | `docs/development/BUILD_WINDOWS.md` |
| 测试结论 | `docs/testing/TEST_MATRIX.md`，但产品断言仍以 `docs/PRODUCT.md` 为准 |

不要为了“了解背景”无差别读取或继续执行归档计划。只在追溯某个具体决策、回归或数据来源时读取 `docs/archive/`。

## 开始编码前必须分类

每项拟议改动先归入以下一类，并在工作说明、提交说明或最终回复中保持可追踪：

1. **上游能力恢复**：恢复相对 `adef6cd` 被误删或退化的行为。
2. **核心能力最小实现**：多入口、AnyTLS 或专用并发端口。
3. **已明确便利功能**：例如产品契约中已列出的分享格式。
4. **局部稳定性修复**：有真实复现，且不改变产品语义的数据丢失、竞态、崩溃或错误处理问题。
5. **范围外候选**：无法映射产品契约，或需要新自动策略、全局 OS 副作用、持久 service、大规模迁移/重构。

第 5 类不得自行实现。发现它可能有价值时，记录问题、证据、影响和最小候选方案，等待用户决定。

“代码很乱”“以后可能需要”“安全上更完整”“已有代码已经做了一半”“测试更方便”都不能单独把范围外候选升级为需求。

## 三项核心能力的执行边界

### Resolver 与人工多入口

- 严格遵守 `proxy-server-nameserver` 字段存在性语义；专用字段 present 与 absent 不能合并处理。
- provider DoH 失败时不得借本机、路由器或公共 DNS 解析线路 server。
- 没有 provider DoH 的普通订阅保持 NekoRay/sing-box 原生解析，不伪造 strict resolver。
- 原始 server domain、resolver 来源、候选 IP、诊断证据、用户固定入口和 TLS SNI 必须分离保存。
- 刷新候选和诊断默认不改变 active entry。
- 固定入口必须由用户明确操作；解除后恢复原 resolver 语义。
- 不移植 MultiMapper 的 `best_ip`、自动 pool round-robin、单次延迟择优、公共 DNS fallback 或第三方 GeoIP 自动决策。

### AnyTLS

- 支持 profile 新建、保存、编辑、复制、导入、订阅、分享、运行和 round-trip，不能只打通某一个入口。
- native、Mihomo compatibility 和继承来源必须显式、可逆。
- AnyTLS 必须兼容 NekoRay 的 group、front proxy、detour、路由、resolver 和专用并发端口模型。
- AnyTLS + Trojan/front proxy 失败先按兼容 bug 调查；不得为跑通删除 chain 或静默降级。
- Xray 不进入运行时和 UI 可选 core，但名称含 `v2ray` 的格式、插件、组件或 sing-box 可承载协议不因此删除。

### 多线路并发端口

- 原生 `127.0.0.1:2080` 保留 NekoRay 正常 Mixed 与路由语义；当前 profile 是默认出站，不是无条件终结规则。
- 新增专用 listener 才是严格线路选择器，绑定稳定 listener/profile 身份与完整 outbound chain。
- 完整 chain 包括 front proxy/detour、resolver、协议 client 类型和必要出站对象，不能只记录最后一跳 tag。
- 端口保存/启动前检查范围、重复和占用；冲突明确失败，不能 try-next。
- 专用线路失败只影响对应端口，不得改投主线、其它线路、`direct` 或 `bypass`。
- 显式 reject/block 可以保留；诊断和协议预处理不得被误写成跨线路 fallback。
- 外置 core 组合暂不能参与并发时，保留 profile 和上游能力，只拒绝该组合。

## 上游兼容与 Xray 删除边界

修改前先用 `git log`、`git diff adef6cd -- <path>`、调用链和现有 profile schema 判断某能力是否来自上游。

默认保留：

- profile/group、ID、顺序、front proxy、chain 和旧配置读取；
- VMess/VLESS/Trojan/SS/SOCKS/HTTP 等由 sing-box 或外置 core 承载的能力；
- v2rayN 分享格式、v2ray-plugin、Qv2ray/NekoRay 兼容组件；
- external-core、Naive、插件、导入导出、路由、测速、Geo 数据和手工系统代理/TUN 操作；
- 暂时无法参与新并发模式的旧 profile 数据。

只允许删除 Xray 运行核心和经调用链证明真正只服务于该核心、且没有格式/数据/外置 core 兼容用途的路径。删除前必须列出：

1. 上游用途；
2. 当前调用方；
3. 与 Xray runtime 的专属性证据；
4. 对旧配置和分享格式的影响；
5. 回归测试。

无法证明专属性时不删。

## RouteFluent 与 MultiMapper 的使用方式

两个外部项目默认是**只读参考**，不是本仓库的子系统，也不授权操作其生产服务器、终端或运行配置。

RouteFluent 可借鉴：

- 显式 listener/线路/chain 映射；
- 端口唯一性和占用预检；
- 业务路径不 try-next、不跨线路 fallback；
- 诊断与运行决策分离。

RouteFluent 不可照搬：

- 绿地项目删除旧兼容的策略；
- OpenWrt Segment/DHCP/TProxy/nftables 模型；
- 全局 strict parser 对 NekoRay 旧数据的破坏性处理；
- 持久网络控制平面作为 NekoRay 核心前提。

MultiMapper 当前没有根级 `AGENTS.md`；等价维护约束位于：

- `D:\python\MultiMapper\README.md`
- `D:\python\MultiMapper\docs\README.md`
- `D:\python\MultiMapper\docs\HANDOFF_FOR_DEVELOPERS_2026-07-14.md`
- `D:\python\MultiMapper\docs\PRODUCTION_MAINTENANCE_BOUNDARIES.md`
- `D:\python\MultiMapper\docs\NEX_WD_ENTRY_OPERATIONS.md`

MultiMapper 可借鉴人工入口选择、来源证据、固定/解除、分层诊断和“单入口失败不等于整线失败”。其字段、UI、OpenWrt deploy、生产库存和自动选择实现都不是 NekoRay 权威模型。

从两个项目复制 YAML、AnyTLS、Trojan 或线路材料时：

1. 只复制当前测试需要的最小 fixture；
2. 先去除真实域名、IP、用户、密码、token、订阅 URL 和证书私钥；
3. 用明确假值替换，并记录来源 commit 与预期语义；
4. 不复制 `.ssh/`、`local/`、`temp/`、日志、抓包、生产 config 或现场审计文件；
5. fixture 不得在测试期间访问真实生产 endpoint。

## 开发流程

### 1. 调查

1. 读取当前分支、状态和已有改动，不覆盖他人工作。
2. 用 `rg` 搜索现有实现、调用方、字段持久化、UI 入口和测试。
3. 同时查看上游基线与当前实现，区分“上游原有”“本项目必要新增”“上一阶段越界新增”。
4. 先写出最小问题陈述、产品条目、受影响文件和不应改变的行为。
5. 用户只要求调查/诊断时保持只读，不顺便实现修复。

### 2. 设计

- 优先复用上游模型和公共方法，不建立第二套 profile、resolver、route 或 runtime 语义。
- 新字段必须说明所有权、默认值、旧数据读取、序列化、复制、订阅刷新和删除行为。
- UI、持久化、ConfigBuilder 和 core 之间对同一概念只能有一套权威表达。
- 能用局部函数、字段或显式映射完成时，不新增 service、全局状态机或跨模块框架。
- 需要用户选择的语义不得由 agent 猜测；列入 `docs/DECISIONS_NEEDED.md` 并停止在安全边界。

### 3. 实现

- 先增加最小失败回归或 fixture，再修改实现。
- 一次变更只处理一个共同验证的问题；恢复上游和新增功能尽量分开。
- 不全文件格式化、不机械改写无关代码、不顺手清理历史命名。
- 不吞错误、不用空成功伪装、不以日志后继续代替失败返回。
- 不因一条 profile 损坏而删除整组，不因一个入口失败而失效整线。
- 订阅刷新必须 parse/stage/validate 后提交；失败保持旧 group 不变。
- 保存失败必须保持内存与磁盘旧状态可解释，不能半应用。
- 当前实现过度复杂时，先用测试固定必须保留的行为，再做可逆的局部简化；不要再发起第二次整体重写。

### 4. 自审

提交验证前逐项检查：

- 该 diff 是否能映射到一个产品条目？
- 是否误改原生 `2080`、普通路由、无 DoH 节点或旧 profile？
- 是否新增自动选择、fallback、try-next、system DNS 或 direct 路径？
- 是否把诊断结果写回运行选择？
- 是否删除了名称含 `v2ray` 但并非 Xray runtime 的能力？
- 是否引入测试机、Clash TUN、OpenWrt 或真实线路特例？
- 是否暴露凭据、订阅、节点地址、完整配置或本地路径资产？
- 是否把当前实现状态误写成产品需求？

发现任一项时先修正，不把它登记成“以后再处理”的低风险债务继续叠加。

## 数据、凭据和本机环境

- 真实 `config/`、groups、订阅正文、分享链接、完整 core JSON、日志、审计报告、证书和凭据不得提交。
- 未知/旧版数据不得静默删除、重排或复用 ID；破坏性迁移前建立可验证备份和回滚。
- 日志和错误信息不得包含密码、token、订阅 URL 或完整分享链接。
- 本机 Clash TUN 是不可中断的外部基础网络。用户已明确要求它始终保持运行；agent、脚本、构建和测试禁止停止、重启、结束其进程，禁止写入或接管其配置、接口、Fake-IP、DNS、路由及其它网络状态。不得默认存在“可暂停 Clash”的维护窗口。
- 本地测试默认不得修改系统代理、TUN、WFP、默认路由、网卡或 DNS。
- 本机只允许不改变 Clash 和 Windows 网络状态的静态检查、构建、纯逻辑、loopback 与只读快照。需要启停本项目 TUN、注入 Windows 网卡/路由/DNS 故障或排除双 TUN 干扰时，必须转移到隔离环境。
- WSL 只可证明 Linux core、配置、协议和 loopback 行为，不能替代 Windows Wintun、系统代理、WFP、驱动、GUI 生命周期或双 TUN 验收。Windows 专属断言应在独立 Windows VM、Windows 沙盒或其它不依赖本机 Clash 的测试机验证；创建或改变这类环境前先提交隔离、快照、网络和清理方案供用户审核。
- WSL、VM 或沙盒不得通过修改本机 Hyper-V/vSwitch、默认路由、DNS、系统代理、Clash 配置或网卡来获取测试条件。无法安全隔离时应标记“未验证”，不得在本机冒险补证据。
- 测试进程只按本次创建并核对的精确 PID/路径清理；禁止按进程名、端口范围或模糊匹配批量结束。
- `deployment/windows64` 中存在运行实例时，正式打包应 fail-fast，不得替用户关闭或强杀。
- OpenWrt 实验只在任务确实需要协议/配置对照时按隔离实验文档执行；不得修改 RouteFluent 或 MultiMapper 的生产 service、配置、nftables、路由和资产。

## PowerShell、路径与 UTF-8

本项目主要在 Windows + PowerShell 环境开发。读取中文和输出中文前使用 UTF-8：

```powershell
$OutputEncoding = [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
Get-Content -LiteralPath 'docs\PRODUCT.md' -Raw -Encoding utf8
```

- 出现乱码时先修复编码并重读，禁止基于乱码做判断。
- 搜索文件优先 `rg` / `rg --files`。
- 修改文件使用补丁工具；不要用 shell 重写整个文件。
- 文件操作使用明确的 `-LiteralPath`；删除/移动前核对解析后的绝对路径。
- 不把构建目录、deployment、外部仓库或生产路径当作临时清理目标。

## 验证要求

验证与改动风险相称，不用一个“大而全”命令掩盖缺失的针对性证据。

### 文档-only

至少执行：

```powershell
git diff --check
.\tools\verify_repository_hygiene.ps1
```

### C++、UI、数据模型或配置生成

- 先运行直接相关的单测/fixture；
- 再在已配置的同轮构建目录执行相关目标和 CTest；
- 需要增量构建时按 `docs/development/BUILD_WINDOWS.md` 设置 MinGW `PATH`；
- 不把 CTest 纯逻辑通过写成真实 GUI、剪贴板、线路或 Windows 网络验收。

常用入口：

```powershell
cmake --build build-package-windows64 --target nekobox --parallel 2
ctest --test-dir build-package-windows64 --output-on-failure
```

### Go core/RPC

只对涉及模块运行，至少包括普通测试；并发或公共逻辑变化补充 race/vet：

```powershell
Push-Location go\cmd\nekobox_core
go test ./...
go vet ./...
Pop-Location

Push-Location go\grpc_server
go test ./...
go vet ./...
Pop-Location
```

### 完整 Windows 打包

只有改动影响打包、真实 core、资源、依赖或交付闭环时，才运行：

```powershell
.\build_windows_package.ps1
```

正式验收不得使用 Skip 参数复用旧 GUI/core。构建脚本成功不等于真实线路或 Windows 网络行为通过。

### 网络能力

- resolver：除单测外，需要对应字段矩阵、断网/失败路径和 DNS 泄漏观测；
- 多入口：验证刷新/诊断不改 active entry，固定 IP 后保留原 SNI；
- AnyTLS：验证 round-trip、单跳、front proxy/detour 和专用端口；
- 并发端口：至少两条线路并发、端口冲突、单线失败隔离、无跨线/direct fallback，并对照原生 `2080` 路由。
- Windows TUN、Wintun、系统代理、WFP、网卡和路由故障注入不得在当前主机执行；WSL/OpenWrt 结果只能证明相应 Linux/core 层，最终 Windows 证据必须来自隔离 Windows 环境。

无法执行某层验证时明确写“未验证”及原因，不能用低层测试替代。

## 文档维护

- `docs/PRODUCT.md` 只在用户明确新增、删除或修正需求时更新；agent 的实现偏好不能写成产品政策。
- 本文件只维护 agent 工作规则，不复制完整产品规格。
- 当前状态和证据写入 `TAKEOVER_STATUS.md`/`KNOWN_ISSUES.md` 时必须标注日期、commit 和证明范围。
- 已失效计划/调查移入 `docs/archive/` 或明确标记 historical/superseded，不能与现行文档并列为权威入口。
- 不创建多个内容重叠的“最终方案”“新契约”或“第二路线图”。
- 文档中的命令、路径和链接必须实际存在；docs-only 变更也要运行链接/仓库卫生检查。

## Git 规则

1. 不回退、覆盖、整理或提交不是本轮产生的改动；同文件已有改动时先理解并合并。
2. 禁止 `git reset --hard`、`git checkout --`、强制 push、历史重写和模糊批量清理，除非用户明确指定目标与影响。
3. 不因工作树脏而擅自 stash、commit 或丢弃用户修改。
4. 每个提交只承载一组共同验证的变化，message 说明产品意图，避免 `update`、`cleanup` 等空泛描述。
5. 提交前显式检查 staged diff，确认无真实配置、凭据、日志、deployment、构建产物和外部材料。
6. 只有用户明确要求时才 commit、push、创建 PR 或合并；普通调查和本地实现不自动产生外部状态变化。
7. 用户已于 2026-08-31 明确要求本项目及时形成远端检查点。此后每个独立、已验证的工作包应提交并推送到当前任务分支，避免跨多轮堆积；推送前仍须检查 staged diff 和远端跟踪关系。该授权不包含向 `main` 推送、force push、改写历史、合并或替他人提交改动。

## 必须暂停并请求用户决定的情况

- 新增自动择优、自动换入口/线路、负载均衡或新的 fallback；
- 删除无法证明只属于 Xray runtime 的上游能力；
- 改变原生 `2080`、系统代理、TUN、路由、DNS 或其它全局 OS 行为；
- 引入 persistent service、WFP、全新 runtime、第二套数据模型或大规模迁移；
- `docs/PRODUCT.md` 第 13 节中的未冻结实现细节会实质改变用户体验；
- 需要操作 RouteFluent/MultiMapper 外部仓库或生产服务器/终端；
- 需要不可逆删除、真实凭据或无法验证的生产材料；
- 当前代码、产品契约和用户最新指令存在无法通过只读调查消除的冲突。

普通、局部、可逆且已在产品契约内的实现不需要反复请求许可，应继续完成并验证。

## 最终回复要求

最终回复简洁说明：

1. 实际改了什么及对应产品条目；
2. 运行了哪些 diagnostics、测试、构建及结果；
3. 哪些层级仍未验证；
4. 是否 commit/push；未得到用户要求时明确保持未提交即可；
5. 下一步最小、可验证的工作是什么。

不要用代码量、测试数量或“架构更完整”替代产品结果，也不要把历史候选方案描述成既定方向。
