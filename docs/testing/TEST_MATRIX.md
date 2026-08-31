# 测试矩阵

状态：现行证据台账；产品断言服从 [产品方向与开发契约](../PRODUCT.md)
最后更新：2026-08-31

## 证据要求

每次发布候选至少记录：提交号、GUI/core 文件哈希、Windows 版本、命令、预期结果、实际结果和脱敏报告路径。日志、导出配置和报告可能含节点名、服务器地址、路由、进程及本地路径，不得提交真实凭据。

Windows x64 是唯一后续验收平台。本机 Clash TUN 必须始终运行，因此当前主机只执行无侵入证据；项目 TUN 和 Windows 网络状态验收转到独立 Windows 隔离环境。Linux/WSL/OpenWrt 旧结果只作为历史背景，不再重跑或计入当前完成证据。

2026-08-31 已启用 Windows Sandbox feature，但 Windows 要求重启后才可首次启动；本轮为保护 Clash 未自动重启。仓库已加入固定禁用 networking 的 `.wsb` 生成、只读白名单 staging、Sandbox runner 和宿主结果核验。当前只证明安装与工具准备，不证明 Sandbox、Wintun、GUI 或项目 TUN 已运行；证据边界见 [Windows Sandbox 隔离验证](WINDOWS_SANDBOX.md)。

历史一次性 Mixed/AnyTLS 调查只在矩阵中作为明确标注日期的诊断背景保留；原始证据见 [2026-07-20 接管基线](../archive/audits/2026-07-20-takeover-baseline.md)。历史中的单次成功不能升级为当前通过结论。

## 2026-08-31 Shadowsocks 分享与 v2ray-plugin 兼容恢复

| 检查 | 结果 | 证明范围 |
|---|---:|---|
| 上游与协议来源对照 | 确认回归 | `11ba168` 删除 legacy whole-payload parser，随后又从 Clash mapping/UI 删除 v2ray-plugin；SIP002、Shadowsocks legacy 文档、v2ray-plugin 和受控 sing-box 源码证明它们不是 Xray runtime |
| 定向红绿回归 | 修复前失败、修复后通过 | SIP002 base64url userinfo、plain userinfo、AEAD-2022 百分号编码、domain/IPv6、备注和 plugin query 解析/导出/reparse；不访问 DNS/网络 |
| legacy 与错误输入 | 通过 | 整段 base64 兼容导入按首个 method colon、最后 server `@` 拆分，password 内 colon/`@` 保留；非法 base64/结构/端口失败；legacy 不作为新导出格式 |
| v2ray-plugin 字段 | 通过 | URI plugin 反斜杠转义 round-trip；Clash `mode/host/path/tls` 生成 SIP003 options，`\`、`;`、`=`、`,`、`:` 在值中转义；UI 重新提供选项，C++ outbound 仍生成 sing-box `plugin/plugin_opts` |
| `nekobox` 增量构建、完整 CTest | 通过、5/5 | parser/export/Clash/UI 编译链接和纯测试通过；不证明真实 WebSocket/TLS/QUIC plugin、订阅事务、GUI 剪贴板或辅助端口运行 |

本恢复使用 sing-box 1.13.12 源码中的内置 `v2ray-plugin` 实现，不下载、打包或启动外置 v2ray/Xray 可执行文件。本机 Clash 和 Windows 网络状态未改变。

## 2026-08-31 VMess v2rayN 分享兼容恢复

| 检查 | 结果 | 证明范围 |
|---|---:|---|
| 上游与格式来源对照 | 确认回归 | `11ba168` 以删除 Xray 遗留为由同时删除 VMess v2rayN parser、export 选择和设置；`adef6cd` 与 v2rayN 官方 schema 证明它是 VMess 分享格式，不是 Xray runtime |
| 定向红绿回归 | 修复前失败、修复后通过 | 标准 base64 JSON 的 `add/port/id/aid/net/host/path/type/scy/tls/sni/alpn/fp/insecure/ps` 解析、导出、重解析保真；`h2` 与内部 `http` 可逆，不访问 DNS/网络 |
| 格式与错误分流 | 通过 | `port/aid` 接受字符串或整数；非法 base64/长度、非 JSON、缺 server/UUID、越界/小数 port 明确失败；现代 `vmess://uuid@server...` 留给原 URI parser |
| 生产接入 | 通过 | `VMessBean::TryParseLink/ToShareLink` 使用同一 helper；设置恢复、默认启用并注册持久字段。带 Reality public key 时继续导出现代 URI，并补齐 URI 的 ALPN 与 TCP-HTTP path round-trip，避免 v2rayN JSON 无表示字段时丢 key |
| `nekobox` 增量构建、完整 CTest | 通过、5/5 | UI/DataStore/parser/export 编译链接和纯测试通过；不是 clean package，不证明真实 GUI 剪贴板、二维码、订阅、远端 VMess 或 Windows 网络 |

当前 v2rayN schema 的 `vcn/pcs` 在 NekoRay Bean/runtime 中没有等价字段，本切片未猜测映射；不把基线恢复写成所有新扩展均已支持。

## 2026-08-31 SOCKS base64 userinfo 上游兼容恢复

| 检查 | 结果 | 证明范围 |
|---|---:|---|
| `adef6cd` 对照 | 确认回归 | 上游在无显式 password 时识别 base64 `user:password`；`11ba168` 删除 legacy Xray 代码时一并删除该非 Xray 格式兼容 |
| 定向红绿回归 | 修复前失败、修复后通过 | 脱敏 `socks://<base64(user:password)>@proxy.example:1080#legacy` 解码并规范导出为 `socks5://user:password@...`，再次解析字段不丢失；不访问网络 |
| 错误输入保留 | 通过 | 显式 password 优先；非法 base64、解码后无冒号、非法 UTF-8 不被猜测或改写；password 中第二个及后续冒号保留 |
| `nekobox` 增量构建、完整 CTest | 通过、5/5 | 生产 `SocksHttpBean::TryParseLink()` 已接入同一严格 helper；证明编译链接和纯测试，不等于 GUI 剪贴板/持久化验收 |

该恢复不引入 Xray runtime，不改变 SOCKS core outbound、resolver、路由、TUN 或系统网络。

## 2026-08-31 分享 server 与 TCP Ping 过度阻止整改

| 检查 | 结果 | 证明范围 |
|---|---:|---|
| ipiptest.org 假数据格式探测 | 通过 server/hostname 语义核验 | 仅提交 `proxy-host.invalid:1080:user:pass`：站点接受格式并进入 hostname DNS lookup；带/不带方括号的文档 IPv6 样本返回 HTTP 400。未提交真实线路或凭据。站点当前同一入口分别报告 SOCKS5/HTTP，VLESS 使用另一链接入口；网页历史标签 `ip:...` 不改变本项目经用户确认的 `server:...` 契约 |
| `share_format_test` 定向构建与 CTest | 通过 | 纯函数证明 `proxy.example`、`proxy-host` 原样形成 `server:port:user:pass`，不触发 DNS；`%N` 凭据不会被 Qt 占位符二次解释；空白/冒号/IPv6 等歧义 server 仍拒绝。未创建 MainWindow，未操作真实剪贴板 |
| `go test ./...`、`go vet ./...`（`go/cmd/nekobox_core`） | 通过 | 新回归在随机 loopback listener 上验证 TCP Ping 真实建立系统 TCP 连接；URL/Full Test 仍要求显式有界配置。未访问外网，不证明 Windows TUN 下的实际路径 |
| 完整本地 CTest | 5/5 | 配置恢复、runtime transition、分享格式、辅助路由编译和 resolver policy 均通过；都是纯/隔离测试，不证明 GUI 或真实网络 |
| `nekobox` 增量构建 | 通过 | 当前 `ShareFormats`、TCP Ping GUI job/tooltip 和中文翻译可由既有 Qt/MinGW 构建目录编译链接；不是 clean build 或完整 package |
| 真实 GUI/剪贴板/远端节点 | 未执行 | 不把纯函数、loopback 和增量构建冒充用户验收 |

本轮没有启动 GUI/core 产品实例，没有修改系统代理、TUN、WFP、路由或 DNS，也没有停止/改写本机 Clash TUN。

## 2026-07-28 主入口与专用端口路由整改

| 检查 | 结果 | 边界 |
|---|---:|---|
| 修复前定向回归 | 14/15 | 新增 `primary_mixed_preserves_native_routing` 后，旧构建只有该用例失败，证明断言命中了既存的无条件主线路绑定 |
| 辅助生成定向回归 | 15/16 | 在实现审计导出前，新增的 ProfileManager/ConfigBuilder 辅助 chain 用例单独失败；旧构建仍能导出并通过 core schema，但没有生成辅助 listener/chain |
| 增量重建 `nekobox` | 通过 | 重新编译 `ConfigBuilder.cpp` 并链接 GUI，不等同于完整 Windows 打包 |
| `test_final_config_guards.ps1` | 18/18 | 普通导出仍只含主线路；显式审计导出从隔离持久化 profile 生成辅助 listener 和两跳 SOCKS detour，保留 inbound-scoped reject/terminal 顺序且不含 TUN、系统代理请求或 `auto_detect_interface`；与 `for_test` 组合会失败 |
| 生成配置 core schema | 通过 | 上述 ProfileManager/ConfigBuilder 最终 JSON 由当前 `nekobox_core check -c` 返回 0；不启动 listener 或远端连接 |
| 辅助端口 reject 编译红绿回归 | 通过 | 旧实现只产生 terminal binding，新 `auxiliary_route_compiler_test` 要求复制三个显式 reject 且不复制 resolve/direct/bypass/其它 outbound；修复前失败、修复后通过 |
| 辅助端口 core schema fixture | 通过 | `test/fixtures/auxiliary-reject-routing.json` 使用文档保留地址，`nekobox_core check -c` 返回 0；不启动 listener 或远端连接 |
| 生成配置双专用端口/AnyTLS+Trojan 回环运行 | 通过 | 提交 `f298d46`：隔离 profile/group/映射/route 经 GUI 审计导出后由 core 启动；A 为 Mihomo-client AnyTLS + Trojan group front proxy，并到达独立 origin（210），B 为 HTTP 单跳（211）；terminal 后的 `bypass` 不能移动 A，reject 不触达 HTTP 目标；停止 Trojan 而保持 AnyTLS server/origin 存活后 A 失败、B 与主/辅三个 listener 均存活 |
| CTest | 5/5 | 配置恢复、runtime transition、分享格式、辅助端口路由编译和 resolver policy 纯测试通过 |

提交 `a3dee71`、`9a328a5`、`55bb799`、`f298d46` 已推送。A AnyTLS + Trojan group front proxy 与 B HTTP 单跳现已完成 ProfileManager/ConfigBuilder → 最终 JSON → core 启动 → 双端口请求的同一闭环；显式 chain profile 仍只完成生成与 schema 检查。本轮没有启动普通 GUI、真实供应商节点或 Windows TUN，也没有执行完整 package；真实远端 profile 与 Windows GUI 集成仍待验证。

## 2026-07-24 端口恢复与 Clash TUN 归因

| 项目 | 结果 | 证明范围 |
|---|---:|---|
| Windows 完整无 Skip 打包 | 通过 | clean reset 并重建 GUI/core；raw core Exit gate PASS；私人配置 219 个文件在打包后恢复 |
| CTest | 4/4 | 配置恢复、runtime transition、分享格式和 resolver policy 纯测试 |
| `test_final_config_guards.ps1` | 15/15 | 默认主 Mixed 断言为 `2080`；原生 WD 与 provider DoH 两类 resolver 路径及 OS 副作用拒绝均通过 |
| `test_mixed_probe.ps1` | 7/7 | HTTP absolute-form、CONNECT、SOCKS5h 进入受控回环目标；额外 listener、系统代理、日志和 origin 状态不变或已释放 |
| `test_runtime_connectivity.ps1` | 正例通过、反例正确拒绝 | HTTP/SOCKS5h 204 正例及错误期望状态反例；不证明真实节点可达 |
| 仓库卫生 | 通过 | 401 个 tracked 文件、12 个 PowerShell、43 个 Markdown、112 个本地链接；零失败 |
| 当前 WD profile 0 导出与 `check` | 通过 | 生成 `127.0.0.1:2080` Mixed，0 个 provider resolver group；证明配置可解析，不证明远端线路健康 |
| 当前真实节点经 Clash TUN | 失败并完成分层归因 | Mixed 三协议均命中 `proxy` 后在 Trojan TLS/AnyTLS session 层失败；进程级物理接口+真实 IP 的一次性 TLS 对照成功，说明 Clash global/Fake-IP 路径会改变结果 |
| OpenWrt 对照 | 未执行 | `192.168.1.7` 无 ARP，SSH 未取得 banner；不能据此判定线路失败 |

本轮最终本地审计快照：`nekobox.exe` SHA-256 `888CBADFB6D308F738D718F32D0789646B58155FA827C19D5DEE256E03AB5F3C`；`nekobox_core.exe` SHA-256 `2FEE446DD5AB0C0A3B374F826D77C21B9145C263B11F05F27282DB5575AD4ED3`；zip SHA-256 `7C37A9E3A52ADE3EA7D97E4AD566E65370E5DB93F8678CE858EC73474F62F7F3`；RouteFluent manifest SHA-256 `06CF3AAF4E2DF52311D7B61FA9D34C44F382BDC654AB4B1DBECAC347B8C724E5`。这些文件位于被忽略的本地 deployment，不是正式 release manifest。

当前私人配置主端口为 `2080`，辅助池仍为 `12100..12299`，既有绑定仍为 `89:12100`、`113:12101`。迁移前副本保存在 deployment 的 recovery 目录，不提交 Git。测试期间没有停止或改写 Clash TUN，也没有启停系统代理或项目 TUN。

## 2026-07-22 无侵入回归

| 项目 | 结果 | 证明范围 |
|---|---:|---|
| 当前源码 Windows 全量 C++/package 本地重编译 | 成功 | 无 Skip 流程先以受保护 helper 清空并重建 GUI build tree，再编译 lifecycle v3、QProcess finished tracker、分享格式测试和 raw integration harness；没有继承旧 CMake cache、object 或手工诊断 core。只证明本机工具链的 clean provenance，不是独立 clean-room release |
| 两个 Go 模块普通测试 | 通过 | core 包含 nil-config/system-fallback/FullTest、lifecycle generation、deadline 准入 fence、Start 取消/发布、Exit `EXITING`/对账与真实 localhost gRPC ACK→GracefulStop；`grpc_server` 覆盖 token + UUID、协议版本 3、one-shot shutdown controller 与 metadata 边界，不是 GUI/Windows 资源集成测试 |
| Go 重复/race/vet | 通过 | core 的 `go test -count=20 ./...`、`go test -race ./...`、`go vet ./...` 通过；`grpc_server` 普通、race、vet 通过。只验证进程内并发断言，不是 GUI/父进程/Windows TUN/WFP 生命周期测试 |
| `test_final_config_guards.ps1` | 15/15 | 隔离导出覆盖原生域名无 provider DoH、有效/过期订阅组 resolver 元数据、域名 DoH 经 `dns-local` bootstrap、非法 DoH 拒绝、bootstrap custom 替换拒绝及既有 OS/路由 guard；不等于 live/test TUN 四象限、完整订阅导入或断网 DNS 泄漏观测 |
| `test_config_preservation.ps1` | 10/10 | 损坏主/路由、错误辅助映射、非法活动路由路径、未知 profile 与悬空 group 引用均保持原件不变并生成可验证隔离证据；显式恢复报告/回滚通过，未完成事务在配置加载前阻断且原件不变 |
| `test_verify_mixed_openwrt.py` | 19/19 | 远端 helper 收紧与命令安全单测；未执行真实远端协议重测 |
| `test_mixed_probe.ps1` | 7/7 | loopback Mixed、拒绝项、额外 listener/系统代理/日志/origin 清理 |
| `test_runtime_connectivity.ps1` | 正例通过、反例正确拒绝 | expected 204 时 HTTP/SOCKS5h 均 204；expected 200 时报告 2 项 mismatch；系统代理、端口与 origin 清理通过 |
| 批量分享格式（当时契约，已于 2026-08-31 修正） | C++ 实现/纯测试当时通过 | 当时错误命名为 `ip:port:user:pass` 并把域名列为负例；这只证明旧实现遵守了旧断言，不再代表现行产品结果。现行 `server:port:user:pass` 证据见本文件顶部 2026-08-31 矩阵 |
| CTest | 4/4 | 在项目 MinGW `bin` 已加入 `PATH` 的环境中通过：配置恢复、runtime transition/finished tracker、分享格式纯函数，以及 WD/NEX resolver 来源、DoH URL/bootstrap/strict group 纯测试。CTest 不创建 QProcess/GUI/core，也不操作系统剪贴板或执行真实 HTTP/2；这些纯测试不代表完整 ProfileManager 刷新、真实 DNS 网络行为或 Windows TUN/WFP 已验收 |
| raw core Exit integration | 完整无 Skip package gate PASS | 用随机 loopback control port、无 listener/无 TUN 配置和刚构建 core，验证 lifecycle v3 握手/deadline、错误 UUID 为 gRPC 16、Exit non-admission 对账 fence/迟到命令拒绝、active Exit 为 gRPC 9、显式 Stop、结构化 `EXITING` ACK、同一 QProcess `NormalExit/0` 和常见 WinINet 五键不变。它不调用产品 `Client::Exit`/MainWindow，不验证生产 PID/`2080`、适配器、路由、DNS、TUN 或 WFP |
| Windows quality CI | 通过 | 仓库卫生、固定子模块、受控 core 源构建、Go 普通测试和 verifier 安全契约；不覆盖 GUI/TUN/WFP |

本轮测试使用受保护 clean reset 后重建的 GUI 和 package core：`build-package-windows64/nekobox.exe` 与 `deployment/windows64/nekobox.exe` 的 SHA-256 均为 `3E918885EBB20D0A00FF04FD43E16841E5C0453CCD324C6F5EDE2BB3C3EBB43D`；core 仅存在于 `deployment/windows64/nekobox_core.exe`，SHA-256 为 `F545DC44627B83DAF49786F3403ED9E464783D71E6917CE06FDFFC0E147D09E5`，不声称 build tree 中存在第二份 core。不带 Skip 参数的完整打包及 tracker/share/resolver policy/raw Exit gate 已成功，zip SHA-256 为 `86F3CD775DFF03B13FF6A66DC225FFA1BDDA0B919D504542384C0D743CFBC306`，package RouteFluent manifest SHA-256 为 `28100CC9F77DE340A3B76A873E476B8EA9D4ECB115B1BA347FFF57345184760A`。215 个 package 配置文件已恢复，没有 preserve 或手工诊断产物残留。这些 hash 只是被忽略的本地审计快照，不是 release manifest。以上均不是 Windows TUN/WFP、系统代理 broker、GUI→Client 退出或线路切换验收。

历史的 2026-07-22 打包/Exit gate 前后快照记录了当时外部 NekoRay 占用 `2080`；该环境已被 2026-07-24 的 Clash TUN 基础网络替代，只保留为历史证据，不再约束当前产品端口。

## 分层验证矩阵

| 级别 | 范围 | 必测项目 | 当前结论 | 发布要求 |
|---|---|---|---|---|
| L1 本地无侵入 | 配置/schema | 每个导出配置执行 `nekobox_core check`；空配置、迁移、损坏配置 | 已验证损坏主/路由配置及错误类型、非字符串、重复辅助映射原件不被覆盖；其它迁移矩阵不完整 | 必须自动化通过 |
| L1 本地无侵入 | Mixed contract | HTTP absolute-form、HTTPS CONNECT、SOCKS5h、认证正反例、端口占用、非 loopback/TUN 拒绝 | 正向及安全收紧有证据，反例不完整 | 必须全部通过 |
| L1 本地无侵入 | 端口映射/OS 副作用 | 主 `2080` 保持上游路由语义；每个专用端口命中其绑定完整 chain；显式 reject/block 可生效；顶层 custom 不得改变专用 listener/outbound 绑定；无明确操作不得改变系统代理/TUN | 2026-07-28 已用导出红绿回归关闭主入口无条件绑定；辅助 reject/terminal 已有纯 C++ golden、显式两跳 chain schema，以及生成 AnyTLS + Trojan group front proxy 与独立 HTTP 单跳线路的回环运行证据。仍缺显式 chain profile 与真实供应商节点验证 | 必须通过 |
| L1 本地无侵入 | 工具安全 | 不改系统代理/TUN/路由/DNS；拒绝 TUN、系统 NTP 写入和非空 endpoints；只保留目标 outbound detour 闭包并只结束精确 PID；不停止或改写 Clash TUN | 启动 GUI/core、写审计报告及构建/临时目录的脚本参数继续使用固定磁盘、非生产/非 reparse 路径护栏；Windows 本地收紧器已有 fixture | 必须保持 |
| L1 本地无侵入 | 遥测一致性 | worker 更新 counter/rate 时 UI/JsonStore 只能读取同一代不可变快照，或由统一锁/原子协议保护；Reset 与持久化也必须纳入 | `last_update=0` 已消除未初始化读取；`TrafficBinding` 只隔离 profile/tag 身份。共享 `TrafficData` 的 counter/rate 仍存在无锁跨线程访问，尚无并发测试 | P2，稳定版前关闭 |
| L3 隔离 Windows 集成 | 生命周期 | GUI 退出/重启、线路重启、core 崩溃；与 4.0.1 对照无额外限制、误杀或数据丢失 | 当前已有 UUID/executor/对账改造，但缺完整 GUI 证据，且复杂度可能超过需求 | 按上游回归审计 |
| L3 隔离 Windows 集成 | 网络控制 | 系统代理、项目 TUN、IPv4/IPv6、DNS 的手工操作与上游回归；状态区分 requested/observed | 当前加入了多项额外 guard；persistent service/WFP 不属于现行核心要求 | 不得比上游退化；不在当前主机执行 |
| L3 隔离 Windows 集成 | GUI/安装更新 | clean build、干净用户目录、安装/更新失败与回滚 | 无 Skip package 先 clean reset GUI build tree、强制 `BUILD_TESTING=ON`，在同轮 GUI tests 与刚构建 core 上依次运行 tracker 和 raw Exit，只有通过才写正式 zip；任一 Skip 只产诊断目录且不创建/覆盖 zip。独立 clean-room 环境、安装/更新失败与回滚矩阵仍未完成 | 阻断稳定版 |
| L1/L3 | C++/Go/脚本自动测试 | CTest、自有 Go 模块与隔离导出 fixture | CTest 为 5 项纯测试，覆盖 tracker、分享格式、辅助 route 编译和 resolver policy；Go 单测覆盖协议 v3 deadline、Start 取消/发布、Exit/对账/finished 顺序，完整 package 另有真实 core raw QProcess/HTTP2 gate。尚无 GUI→Client crash→commit、真实 timeout/ACK 丢失、分享剪贴板、TrafficData 并发或 ProfileManager/订阅完整 harness。两个 Go 模块普通测试通过，core module 的重复/race/vet 通过；PowerShell 已覆盖 ConfigBuilder 的 AnyTLS + Trojan group front proxy 与 HTTP 单跳辅助线路运行，但完整 import、显式 chain profile、真实供应商组合和 DNS 泄漏观测仍缺 C++/Windows 集成矩阵 | 阻断稳定版 |

共享路径护栏只是 fail-closed 的路径前置筛选：它没有通过最终文件句柄验证 final-file identity，不能声称已解决所有 hardlink 或其它同文件别名。报告/导出工具 `export_profile_core_config.ps1`、`verify_runtime_connectivity.ps1` 和 `verify_fail_closed_restart.ps1` 会拒绝覆盖已存在的目标文件；此项必须与路径护栏分开测试，且不得当作完整文件身份保证。

## 逻辑线路与接口选择的断言

- 默认 Mixed 端口保持上游值 `2080`；专用端口范围是可调整的实现参数。
- 主 `2080` 必须保留上游路由/reject/DNS 语义；专用线路入口才强绑定完整 profile chain，并拒绝改投 `direct`、主线或另一条线路。
- 专用入口的断言对象不仅是 tag/port/detour：完整 listener 与全部可达 outbound 必须在 custom merge 后仍保持绑定。该断言不得遮蔽主入口的上游规则。
- `auto_detect_interface` 仅是底层 OS 接口选择，不能改变入口到逻辑线路的绑定，也不能作为“Mixed 自动选线路”的验收依据。
- 本机 Clash TUN 存在时，本地远端成功只能证明请求完成，不能单独证明物理出站归属，也不能排除代理套代理/Fake-IP 影响。此时按 [Clash TUN 共存](../operations/CLASH_TUN_COEXISTENCE.md) 转到隔离 Windows 环境。

L2 历史诊断细节见[已验证基线](OPENWRT_REMOTE_LAB.md#已验证基线)。2026-07-20 的旧探针对每个临时变体强制 `auto_detect_interface=true`；它只能在这个共同条件下把故障收敛到 AnyTLS mihomo client 与 `g-2` detour 的组合，不能证明产品导出接口策略。必须按当前默认 preserve 重跑后才能成为现行证据，更不证明 Windows Wintun、WFP、系统代理、GUI 或线路重启已通过。

## 发布前最小矩阵

1. 普通无网络副作用的 GUI/数据测试使用干净 Windows 用户目录且不复用唯一真实配置；凡涉及项目 TUN、系统代理、网卡、路由或 DNS 的测试只在独立 Windows 隔离环境执行。本机 Clash 始终保持运行且不被改写。
2. 覆盖空配置、旧配置迁移、损坏配置和恢复流程。
3. 完成 Mixed 三种协议、认证正反例、端口占用、非法端口与主/辅端口映射。
4. 在隔离 Windows 中完成 Trojan、AnyTLS 及有/无 front proxy；历史 Linux 结果不计入当前发布候选。
5. 在隔离 Windows 中对照 `adef6cd` 验证手工系统代理/项目 TUN、GUI 退出、重启和线路切换；不得把旧 persistent 数据面语义当成上游要求。
6. 分别核对 TUN requested、worker-observed 和 Windows 实际状态；任何显示或 RPC 结果都不能冒充接口/路由已经成立。
7. 对 U-005/U-006 的每个额外 guard 建独立四象限回归，只移除或缩窄被证明超出上游/产品的阻止；不以一次“大撤 guard”处理。
8. 覆盖本项目实际拥有的 worker 正常/异常退出、精确 PID/路径清理和旧配置保留；不得结束 Clash 或其它安装。Runtime Service、BFE restart、persistent WFP 和全机防泄漏不是现行发布门。
9. 覆盖空订阅、HTML 错页、超时、解析失败和正常更新；失败时旧组原样保留。
10. 覆盖更新失败、校验失败和回滚；修复更新器前不得进行真实更新测试。
11. 对 GUI→Client→core 覆盖正常退出、异常进程退出、重复关闭及当前保留机制对应的 ACK/finished 顺序；要求不误杀其它进程、不损坏数据且状态可解释。若简化现有 continuation fence，应以同一失败回归证明收益，不把具体 RPC 结构写成永久产品要求。

## 工具入口

- `tools/export_profile_core_config.ps1`：导出单个 profile 的实际 core 配置并可执行 `check`；显式 `-IncludeAuxiliaryAudit` 会把已保存的辅助 listener/chain 一并生成，但不会启动它们。
- `tools/verify_mixed_inbound.ps1`：只接受主 `mixed-in -> proxy` 连通性诊断；拒绝 TUN/系统 NTP 写入/endpoints、裁剪到 `proxy` 的精确 detour 闭包后启动临时配置，验证监听 PID 和三种代理请求；不是辅助映射 contract 或 Windows 集成验收器。
- `tools/verify_mixed_openwrt.py`：历史 Linux 探针；按用户最新要求不再执行。
- `test/fixtures/mixed-direct-sanitization.json`：验证诊断脚本不会启动额外 LAN inbound/controller、修改系统代理或写配置指定日志。
- `test/test_mixed_probe.ps1`：用 loopback HTTP 204 origin 运行 direct、dummy auth 与安全拒绝 fixtures，并核对端口、系统代理、文件副作用和 origin 清理；不依赖公共站。2026-07-20 为 7/7。
- `test/test_verify_mixed_openwrt.py`：历史工具单测；不再纳入后续 Windows-only 验证命令。
- `test/test_runtime_connectivity.ps1`：在临时 package 和 loopback HTTP 204 origin 中验证运行快照脚本的 PID 归属、精确 HTTP 状态、错误期望拒绝及清理；2026-07-20 的 204 正例通过，错误期望 200 正确失败。
- `test/config_recovery_test.cpp`：覆盖单/多文件恢复基础设施、`VerifiedBefore`/`VerifiedAfter`/`Indeterminate`、退役、hidden/unexpected/exact-case/staging，以及 terminal startup/report 分层校验。路径用例 `routes_box/ROUTE~1` 只证明 `~` 被词法规则拒绝，不表示测试构造了真实 Windows 8.3 alias；选定配置根本身的 junction/别名仍需操作者确认。
- `test/runtime_transition_test.cpp`：覆盖 GUI process-local transition/queue/crash generation 竞态、daemon generation 与 UUID 同锁快照，以及 `{generation, UUID, PID}` finished tracker 的错误身份/PID拒绝、异常完成、重复 finished 和 finished-before/after-wait；它不创建 QProcess、GUI、core 进程，不执行真实 HTTP/2，也不覆盖 `TrafficData` 并发或 Windows TUN/WFP。
- `test/share_format_test.cpp`：只用假凭据覆盖原生链接 fragment 精确删除、IPv4/域名/主机名 server 原样输出、端口、SOCKS5/HTTP、TLS、认证与分隔符正负例；纯函数不解析 DNS，不创建 MainWindow 或操作真实剪贴板。
- `test/core_exit_integration_test.cpp`：只由完整无 Skip package 授权运行；再次校验 fresh package core 的规范路径/SHA-256/非生产 identity，以 `NoProxy` raw Qt HTTP/2 client 和 test-owned QProcess 验证协议 v3 deadline/non-admission 对账 fence、迟到 Exit 拒绝和正常退出。配置无 listener、无 TUN；失败清理先尝试已鉴权 Stop/Exit，最后才可 terminate/kill 精确 test-owned PID。它不是 GUI→Client E2E，只比较 WinINet 的 `ProxyEnable`、`ProxyServer`、`AutoConfigURL`、`ProxyOverride`、`AutoDetect` 五键。
- `go/cmd/nekobox_core/core_lifecycle_test.go`：覆盖失败/取消 candidate、blocked Close、旧 reference、dial/stats/Stop 互斥、并发 Start、deadline 准入 fence、Exit STOPPED 前置与终态 `EXITING`；另覆盖 reconcile barrier 先挡迟到 Start/Exit、等待阻塞 Start/Stop、精确 active/failed-clean/blocked target、config hash 与 ordering watermark。`grpc_box_test.go` 覆盖 deadline/Exit/对账映射；`grpc_exit_integration_test.go` 通过真实 localhost gRPC 验证排队 Stop deadline 和 ACK 交付后 GracefulStop。`go/grpc_server/auth` 和 `grpc_identity_test.go` 覆盖 token + daemon UUID、协议 v3、one-shot shutdown controller、metadata 清除和握手回显。
- `go/cmd/nekobox_core/internal/boxapi/boxapi_test.go`：除无 instance fail-closed 外，覆盖 generation-bound HTTP transport 禁用 keep-alive，防止连接跨代复用。
- `test/test_config_preservation.ps1`：在隔离 appdata 中启动配置导出路径，验证已存在但损坏的主/路由配置、重复/非字符串/错误 JSON 类型的辅助映射、非法活动路由路径、未知 profile 类型与悬空 group 引用保持 SHA-256 不变，并验证 snapshot/metadata；同时验证显式事务报告与 before 回滚，以及 pending 事务在加载前阻断且主配置不变，当前为 10/10。
- `test/test_final_config_guards.ps1`：在隔离临时 appdata 中验证安全 `internal-full` 文件导出、部分 TUN/系统代理/system endpoint/NTP 副作用拒绝，以及标准 SOCKS profile 的测试态 custom、主 Mixed 原生路由、辅助两跳 chain 审计生成、detour 禁改和 custom route 空字段回归；当前为 18/18。辅助用例可调用当前 core `check`，但不启动线路，也不覆盖 live/test 的 TUN on/off 四象限或所有 NTP/direct-action dialer，不能代表完整 C++ golden。
- `test/test_auxiliary_route_runtime.ps1`：在隔离 appdata 中持久化主 HTTP、A AnyTLS、A Trojan group front proxy、B HTTP profile、映射和 route，经 GUI 审计导出后启动当前 core；临时自签证书、独立 AnyTLS/Trojan server core 与 HTTP origin 只使用回环。它验证 A 经真实协议 detour 返回 210、B 返回 211、terminal 后不跨线、reject 不触达 HTTP 目标，以及 Trojan 失效时不绕过仍存活的 AnyTLS server/origin 且不影响 B；主端口仅在该隔离配置中改为 `18119`，不改变默认 `2080`。它不代表显式 chain profile 或真实供应商节点。
- `tools/verify_runtime_connectivity.ps1`：采集整套运行时快照；不是自动验收器。
- `tools/verify_fail_closed_restart.ps1`：采集 fail-closed 相关状态；使用限制见 [fail-closed 验证](FAIL_CLOSED.md)。

这些工具调用的 `nekobox_core check/run` 是[显式高级 CLI](../reference/CLI.md#core-高级-cli)，会直接读取经过导出或收紧的临时配置；它们不走普通 GUI，也不能用于证明 Go 层已经独立实施产品策略。
