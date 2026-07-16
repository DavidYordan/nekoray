# MultiMapper 导出配合与多线路运行整改调查

日期：2026-07-16

## 调查范围

本次只做调查和规划，不改业务代码。已阅读：

- 本项目 `docs/ANYTLS_CLASH_IMPORT_AND_RESOLVE_REWORK_2026-07-16.md`。
- 本项目导入、导出、启动、系统代理、TUN、配置生成相关源码：
  - `sub/GroupUpdater.cpp`
  - `fmt/AbstractBean.cpp`
  - `fmt/Bean2Link.cpp`
  - `ui/edit/dialog_edit_group.cpp`
  - `ui/mainwindow.cpp`
  - `ui/mainwindow_grpc.cpp`
  - `db/ConfigBuilder.cpp`
  - `main/NekoGui_DataStore.hpp`
- MultiMapper 当前顶层文档：
  - `D:\python\MultiMapper\docs\README.md`
  - `D:\python\MultiMapper\docs\HANDOFF_FOR_DEVELOPERS_2026-07-14.md`
  - `D:\python\MultiMapper\docs\CURRENT_RUNTIME_STATE.md`
- MultiMapper 相关源码：
  - `D:\python\MultiMapper\base_tab.py`
  - `D:\python\MultiMapper\routing_tab.py`
  - `D:\python\MultiMapper\singbox_config.py`
  - `D:\python\MultiMapper\anytls_client.py`

## 结论先行

1. MultiMapper 当前只面向 RouteFluent sing-box 运行态，不再自动兼容旧 Xray 或 stock sing-box。它的当前约束包括：AnyTLS client 默认 `mihomo/1.19.28`、server 域名解析走 RouteFluent runtime DoH、fallback 默认 `strict`、业务 outbounds 不允许 `direct` 偷跑。
2. 本项目未来给 MultiMapper 的剪贴板导出，不应导出原始 Clash 全量 YAML。原始订阅可能很大，包含 Clash 分组、规则、dashboard、health-check、fake-ip 等 MultiMapper 不需要的运行时信息。
3. 正确方向是新增一个“精简线路包”导出：保留单条线路连通所需字段、来源标识、订阅级 DoH、订阅级 client 默认值，以及必要的单线路 override；丢弃 Clash 分组和规则。NekoRay 自身仍保留本项目数据库里的分流和路由，不把 Clash 分流作为本项目运行时来源。
4. 本项目当前已支持多选复制普通协议链接和 Neko 链接，也支持复制当前组全部链接，但没有“MultiMapper 精简 JSON 包”，也没有“整个订阅以精简包导出到剪贴板”的语义。
5. 本项目当前运行模型是单主线路：一个 `started_id`、一个 core 配置、一个主 `mixed-in`、可选一个 `tun-in`。多条线路同时启动需要新增运行模型，不能只复用现有“启动当前选中线路”。
6. 系统代理和 TUN 的无偷跑要求需要拆开看：系统代理在普通线路重启时不会被 `neko_stop()` 清空，但程序重启/退出路径会调用 `ClearSystemProxy()`；内部 TUN 在 core Stop/Start 期间会被 sing-box 配置卸载，存在回落到系统默认路由的窗口。后续整改必须把“显式关闭代理模式”和“为了重载配置而重启核心”区分开。
7. 一个订阅链接应默认视为一套来源。Clash 订阅无论内部线路协议是什么，都应有订阅级 Mihomo client 默认值；DoH、fallback、订阅信息也应优先在订阅层级统一管理，再允许少量线路覆盖。

## MultiMapper 当前需要什么

MultiMapper 文档和源码显示，它不是通用订阅转换器，而是面向 OpenWrt 终端的 RouteFluent sing-box 生产配置工具。

它的线路数据大致分两类：

- `bridges.json`：桥接线路。
- `outbounds.json`：最终出口线路。

二者文件结构相同，都是按 `source_tag` 分组：

```json
{
  "version": 1,
  "groups": {
    "source-tag": {
      "source": "订阅来源或说明",
      "doh_nameservers": [
        "https://example.com/dns-query"
      ],
      "subscription_info": {},
      "items": []
    }
  }
}
```

MultiMapper 的关键语义：

- Clash YAML 导入时会提取 `dns.proxy-server-nameserver`，没有时回退到 `dns.nameserver`，只保留 HTTPS DoH，并按 `source_tag` 保存。
- RouteFluent sing-box 配置生成时，会按 `source_tag` 为每组线路生成 runtime DoH resolver。
- 若线路 server 是域名，生成 outbound 时会绑定 `domain_resolver`，使该线路 server 由本来源 DoH 解析。
- AnyTLS client profile 默认是 `mihomo/1.19.28`，也允许 profile 级 custom。NekoRay 导出时应把 Clash 订阅的 Mihomo 默认作为订阅级元数据带过去；非 AnyTLS 线路不直接消费该字段，但仍属于同一套订阅默认。
- MultiMapper 不需要 Clash 的分组、规则、负载均衡、url-test、select 运行态；它自己有路由 profile、bridge/outbound 选择、DNS 解析和部署语义。

这说明本项目和 MultiMapper 对接时，最有价值的信息不是 Clash 原文，而是：

- 线路协议和连接字段。
- `source_tag` / `source`。
- 订阅或 provider 级 DoH。
- AnyTLS client 设置。
- 可选订阅流量/过期元数据。

## 本项目当前导出能力

现状：

- 主窗口 `on_menu_copy_links_triggered()`：多选线路复制普通协议分享链接。
- 主窗口 `on_menu_copy_links_nkr_triggered()`：多选线路复制 `nekoray://` 链接。
- 分组编辑框 `copy_links`：复制当前组全部普通协议链接。
- 分组编辑框 `copy_links_nkr`：复制当前组全部 `nekoray://` 链接。
- `on_menu_export_config_triggered()`：导出单条线路生成后的 core config，不是订阅导出。

现有限制：

- 普通协议链接足够轻量，但字段有限。AnyTLS 链接可以携带 `anytls_client_mode` / `anytls_client_value`，但不会携带 `server_resolver_doh`。
- `nekoray://` 链接能携带 bean JSON，信息完整，但 MultiMapper 当前不会直接导入。
- 当前没有“按订阅或分组导出精简 JSON 包”的功能。
- 当前没有记录“这条线路来自哪个原始 Clash proxy group”的必要语义；从 MultiMapper 角度也不需要该语义。

## 订阅层级管理建议

一个订阅链接通常代表同一服务商、同一账号、同一套线路运行假设。后续本项目不应只把订阅拆成孤立线路，而应在分组/订阅层级保存统一默认值。

建议订阅层级保存：

- `source_type`：`clash`、`plain_links`、`base64_links`、`manual`。
- `client`：Clash 订阅默认 `mihomo/1.19.28`。普通链接订阅不自动套 Mihomo，除非用户显式设置。
- `server_resolver`：订阅 DoH、fallback 策略、是否允许本地 fallback。
- `subscription_info`：流量、过期时间、更新时间。
- `export_policy`：是否允许导出订阅 URL、是否导出单线路 override。

建议线路层级保存：

- `inherit_subscription_client`：默认 true。
- `inherit_subscription_resolver`：默认 true。
- `client_override`：只有用户明确单线路覆盖时才保存。
- `resolver_override`：只有用户明确单线路覆盖或导入时发现 per-proxy resolver 且用户选择保留时才保存。

UI 建议：

- 分组/订阅编辑面板增加 `Client`、`Server Resolver`、`Fallback`。
- 支持“应用到继承线路”“覆盖全部线路”“只作为新默认值”三种操作。
- 单线路编辑面板明确显示“继承订阅”或“覆盖订阅”，避免用户不知道当前线路到底用哪个 DoH/client。
- 导入 Clash 时，顶层 `dns.proxy-server-nameserver` 或 `dns.nameserver` 应成为订阅默认 DoH；单 proxy `server-resolver` 默认记录为高级差异，不应自动把一个订阅切碎成多个行为不同的小订阅。

## 推荐剪贴板导出格式

建议新增 `nekoray-multimapper-export-v1` 精简 JSON 包。它不是原始 Clash，也不是完整 NekoRay 数据库，只承载 MultiMapper 需要的信息。

建议结构：

```json
{
  "format": "nekoray-multimapper-export",
  "version": 1,
  "exported_at": "2026-07-16T00:00:00+08:00",
  "groups": {
    "NEX": {
      "source_type": "clash",
      "source": "subscription name or url hash",
      "defaults": {
        "client": {
          "mode": "mihomo",
          "value": "mihomo/1.19.28"
        },
        "server_resolver": {
          "mode": "doh",
          "doh_nameservers": [
            "https://example.com/dns-query"
          ],
          "fallback": "strict"
        }
      },
      "doh_nameservers": [
        "https://example.com/dns-query"
      ],
      "subscription_info": {
        "upload": 0,
        "download": 0,
        "total": 0,
        "expire": 0
      },
      "items": [
        {
          "protocol": "anytls",
          "address": "server.example.com",
          "port": 443,
          "tag": "USA Los Angeles 01",
          "settings": {},
          "streamSettings": {},
          "anytlsSettings": {},
          "inherit": {
            "client": true,
            "server_resolver": true
          },
          "nekoray": {
            "profile_id": 12
          }
        }
      ]
    }
  }
}
```

字段建议：

- `format`：固定值，用于 MultiMapper 识别剪贴板内容。
- `groups`：按 NekoRay 分组或订阅来源组织。导出“选中线路”时，也应保留每条线路原来的来源分组。
- `source_type`：导入来源类型。Clash 订阅必须写 `clash`，用于 MultiMapper 和后续诊断识别默认 client 语义。
- `source`：订阅 URL 不建议明文强制导出；可以导出组名、订阅名、URL hash，是否导出 URL 由后续 UI 选项决定。
- `defaults.client`：订阅级 client。Clash 订阅默认 `mihomo/1.19.28`，不因线路协议不是 AnyTLS 而消失。
- `defaults.server_resolver`：订阅级 server resolver。优先来自 Clash 顶层 `dns.proxy-server-nameserver`，否则回退 `dns.nameserver`。
- `doh_nameservers`：为兼容 MultiMapper 当前 group metadata，保留与 `defaults.server_resolver.doh_nameservers` 相同的 DoH 列表。后续 MultiMapper 支持 `defaults` 后可逐步收敛。
- `subscription_info`：如果本项目保存了订阅流量/过期信息，可带上；没有则省略。
- `items`：尽量贴近 MultiMapper 当前 `bridges.json` / `outbounds.json` item schema，降低 MultiMapper 侧接入成本。
- `inherit`：声明该线路是否继承订阅级 client 和 resolver。默认 true；只有用户明确单线路覆盖时才写 override 字段。
- `nekoray`：仅放辅助元数据，不作为 MultiMapper 生产配置的强依赖。

不建议导出：

- Clash `proxy-groups`。
- Clash `rules` / `rule-providers`。
- Clash `dns` 的分流、fake-ip、nameserver-policy。
- Clash dashboard、external-controller、secret。
- health-check、url-test、load-balance/select 状态。
- 本项目本机系统代理、TUN、测试结果、UI 排序等本地运行状态。

原因：

- MultiMapper 没有 Clash 分流问题，它的分流由自己的 routing profile 和 OpenWrt runtime 决定。
- 本项目需要保留分流，但应保留在 NekoRay 自己的路由数据库中，不应为了导出给 MultiMapper 而保存 Clash 的完整运行时。
- Clash 分组语义映射到 MultiMapper 会有损，也容易让后续维护者误以为 MultiMapper 会复刻 Clash runtime。

## 导出入口建议

建议在本项目增加三个剪贴板入口：

1. 右键选中线路：`复制到 MultiMapper`。
   - 对当前多选线路导出精简包。
   - 同时保留已有 `Copy Links` 和 `Copy Neko Links`。

2. 分组编辑面板：`复制本组到 MultiMapper`。
   - 导出当前组全部线路。
   - 适合用户把一个订阅导入后整体交给 MultiMapper。

3. 订阅分组右键或管理面板：`复制订阅到 MultiMapper`。
   - 如果该组来自订阅，导出本组全部线路和订阅级 DoH/订阅信息。
   - 不导出原始 Clash 全量 YAML。

同时建议保留一个高级选项：

- `包含订阅 URL`：默认关闭。开启时才把原始订阅 URL 写入 `source` 或 `source_url`，避免剪贴板和文档里无意泄露凭据。

## MultiMapper 侧后续配合点

本次不向 `D:\python\MultiMapper\docs` 写文档，待审核后再单独出具给 MultiMapper 开发者的接口文档。

后续建议 MultiMapper 增加：

- 识别剪贴板 JSON 中 `format == "nekoray-multimapper-export"`。
- 让用户选择导入到 `bridges` 还是 `outbounds`。
- 按 `groups` 恢复 `source_tag`、`source`、`doh_nameservers`、`subscription_info`。
- 读取 `defaults.client` 和 `defaults.server_resolver`。在 MultiMapper 还未支持 `defaults` 前，可继续把 `doh_nameservers` 作为兼容字段使用。
- 对 `items` 逐条复用现有 `add_nodes_from_subscription()` 或等价逻辑。
- 对 Clash 来源的 AnyTLS，若线路没有显式 override，使用 group `defaults.client`；非 AnyTLS 线路保留来源默认但不写无效 outbound 字段。
- 对 Nekoray 导出的 per-line provider DoH，如果和 group DoH 不一致，应默认保持订阅统一值，并把 per-line 差异作为高级 override 提示；只有用户明确选择时才拆分 `source_tag` 或启用 override。

## 解析为 IP 与多线路关系

右键“解析为 IP”应从单一动作升级为可诊断的解析任务。原因是同一个 server 域名通过不同解析服务和不同出站路径会得到不同结果：

- Clash 订阅自带 DoH 可能返回 provider 期望的入口 IP。
- 公共 DNS/公共 DoH 可能返回通用公网入口。
- 本机系统 DNS 可能被 Windows、运营商、旧版 NekoRay TUN 或其它代理影响。
- 通过某条代理线路访问 DoH 时，DoH endpoint 看到的是代理出口网络，而不是本机网络。

建议 UI 拆成两个下拉维度。

解析服务：

- `订阅 DoH`：当前线路所属订阅默认 DoH。
- `线路覆盖 DoH`：单线路 override DoH。
- `路由 Remote DNS DoH`。
- `路由 Direct DNS DoH`。
- `公共 DoH`。
- `系统 DNS`。
- `自定义 DoH`。

出站路径：

- `不通过本项目代理`：解析请求不使用本项目 core 的主端口或辅助端口；但这不保证绕过系统级 TUN。
- `通过主线路`：使用当前主线路端口发起 DoH 请求。
- `通过辅助线路`：多线路运行后，列出每个已启动辅助端口。
- `通过临时线路`：后续高级能力，为未运行线路临时启动 resolver-only 配置。

约束：

- `系统 DNS` 只能配合“不通过本项目代理”。系统 resolver 没有可靠的“指定某条代理线路”语义。
- 通过代理解析时优先只支持 DoH，不做 UDP DNS over SOCKS 的隐式兼容。
- 如果用户选择通过代理解析，必须明确选择具体线路，不能默认套主线路。
- 解析结果必须显示解析服务、DoH URL、出站路径、代理线路名称、监听端口、全部 IP、首选 IP、耗时和错误信息。

默认建议：

- Clash 订阅线路默认使用“订阅 DoH + 不通过本项目代理”。
- 如果用户正在诊断 provider DoH 在不同出口下的差异，应切换到“订阅 DoH + 通过指定主/辅助线路”。
- 多线路辅助端口完成后，解析对话框必须实时读取主线路和辅助线路列表；所选线路停止时任务应失败，不应自动切换到其它线路。

## 多线路启动现状

本项目当前运行模型是单主线路：

- `main/NekoGui_DataStore.hpp` 只有一个 `started_id`。
- `ui/mainwindow_grpc.cpp::neko_start()` 启动新线路前，如果已有 `started_id >= 0`，会先调用 `neko_stop(false, true)`。
- `db/ConfigBuilder.cpp` 只生成一个主 `mixed-in`，端口是 `inbound_socks_port`。
- 内部 TUN 打开时，只额外生成一个 `tun-in`。
- 路由规则围绕当前线路的 `proxy`、`bypass`、`direct` 标签生成。
- Clash API、流量统计、UI 运行标识也都假设只有一个当前运行线路。

这意味着“允许启动多条线路”需要新增运行模型，而不是在现有 `started_id` 上叠加列表。

## 多线路运行建议设计

建议把线路分成两类：

- 主线路：仍由现有 `Start` 管理，承载 Windows 系统代理、主 mixed 入站和内部 TUN。
- 辅助线路：不接管系统代理和 TUN，只监听本机额外端口，供 curl、浏览器插件、其它程序或测试工具显式使用。

建议新增持久模型：

```json
{
  "primary_profile_id": 12,
  "aux_listeners": [
    {
      "profile_id": 34,
      "enabled": true,
      "listen": "127.0.0.1",
      "port": 12134,
      "type": "mixed",
      "name": "USA Los Angeles 02"
    }
  ]
}
```

配置生成建议：

- 主线路继续生成 `mixed-in` 和可选 `tun-in`，出站 tag 继续叫 `proxy`。
- 每条辅助线路生成独立 inbound，例如 `aux-mixed-34`。
- 每条辅助线路生成独立 outbound chain，例如 `aux-proxy-34`。
- route rules 最前面添加 `inbound: aux-mixed-34 -> outbound: aux-proxy-34`。
- 主线路的系统代理和 TUN 不受辅助线路影响。
- Clash API 和流量统计应把辅助 outbounds 纳入可观测列表，但 UI 上要区分主线路与辅助线路。

端口建议：

- 主端口继续使用 `inbound_socks_port`，当前默认 `12080`。
- 辅助端口建议从 `12100` 或用户配置的端口池开始自动分配。
- 默认只监听 `127.0.0.1`，除非用户明确允许 LAN。
- 分配端口时必须避开：
  - 主 `inbound_socks_port`
  - sing-box Clash API 端口
  - nekobox_core gRPC 端口
  - 已启用辅助监听端口
  - 当前系统占用端口

UI 建议：

- 右键线路增加 `作为辅助端口启动`、`停止辅助端口`、`复制辅助代理地址`。
- 状态栏显示主线路和辅助线路数量。
- 线路表可增加辅助运行状态标记，例如 `Main` / `Aux:12134`。
- 主线路切换不应自动停止辅助线路，除非用户选择“重建全部运行配置”或辅助线路依赖主线路链式前置。

## 系统代理与 TUN 偷跑风险

当前代码路径：

- 普通线路重启：`neko_start()` 会先 `neko_stop(false, true)`，此路径不会调用 `ClearSystemProxy()`。因此系统代理设置通常仍指向本机端口，应用不会因为系统代理被清空而直接回落；但 core stop/start 期间本机端口不可用，可能表现为连接失败。
- 程序退出/重启：`on_menu_exit_triggered()` 会调用 `neko_set_spmode_system_proxy(false, false)` 和 `neko_set_spmode_vpn(false, false)`。这会清空 Windows 系统代理，并停止外部 TUN。若这是软件重启、管理员重启或更新重启，就存在系统流量回落默认出口的风险。
- 内部 TUN：TUN 是 sing-box 配置中的 `tun-in`。当 `neko_stop()` 通过 gRPC Stop 停止当前配置时，TUN 入站会随配置卸载，系统路由可能回落默认出口。此风险不能仅靠“不清空系统代理”解决。
- 内部 TUN 设置变化：`neko_set_spmode_vpn()` 在 `vpn_internal_tun && started_id >= 0` 时会调用 `neko_start(started_id)`，实际也会触发 stop/start。

目标语义：

- 只有用户明确执行“关闭系统代理”时，才允许调用 `ClearSystemProxy()`。
- 只有用户明确执行“关闭 TUN”时，才允许停止 TUN 或撤销 TUN 相关保护。
- 配置重载、切换线路、软件自重启、管理员提权重启、核心更新重启，都不应把系统恢复成默认直连出口。

建议新增操作原因枚举：

```cpp
enum class ProxyModeTransitionReason {
    UserDisable,
    ProfileReload,
    ProfileSwitch,
    AppRestart,
    AppExit,
    CoreCrashRecovery,
    AdminElevationRestart
};
```

系统代理整改建议：

- `neko_set_spmode_system_proxy(false)` 只在 `UserDisable` 下调用 `ClearSystemProxy()`。
- `AppRestart`、`AdminElevationRestart`、`ProfileReload` 不清空系统代理，只保持 OS 代理指向稳定的本机主端口。
- 如果软件退出但用户没有显式关闭系统代理，系统代理宁可保持指向本机端口，形成 fail-closed，也不要恢复直连。
- 软件启动时，如果发现记忆状态要求系统代理开启，应先设置系统代理到主端口，再启动/恢复 profile。

TUN 整改建议：

- 短期：所有会重启内部 TUN core 的操作都标记为高风险，并在规划中改为 fail-closed 流程。
- 中期：优先研究 sing-box/nekobox_core 是否能支持不停 TUN 的配置热更新。如果不能，必须承认 stop/start 会卸载 TUN。
- Windows 上若必须重启 TUN，应设计临时 kill-switch：重启窗口内阻断非 core 进程默认出站，待新 TUN ready 后解除。该方案需要管理员权限和严格回滚。
- 辅助线路启动/停止不应触发 TUN 重建。只有主线路或路由/TUN 参数变化才允许触发 TUN 相关流程。

## 建议整改阶段

阶段 1：导出契约落地。

- 新增订阅/分组层级默认值模型：`source_type`、`defaults.client`、`defaults.server_resolver`。
- Clash 订阅导入时，无论内部线路协议是什么，订阅默认 client 都是 `mihomo/1.19.28`。
- 订阅层级 UI 支持统一查看、修改和批量应用 client/DoH/fallback。
- 新增 `nekoray-multimapper-export-v1` 构造函数。
- 支持选中线路导出到剪贴板。
- 支持当前组/订阅导出到剪贴板。
- 导出订阅级 provider DoH、订阅级 client、source_type、source_tag/source、单线路 override，默认不导出订阅 URL。
- 保留现有普通链接和 Neko 链接导出入口。

阶段 2：给 MultiMapper 出接口文档。

- 待本文审核后，在 `D:\python\MultiMapper\docs` 新增对接文档。
- 明确 JSON schema、导入行为、DoH 冲突策略、AnyTLS client 默认策略。
- 不直接改 MultiMapper 代码，除非后续明确授权。

阶段 3：多线路辅助端口。

- 新增辅助监听数据模型。
- 配置生成器支持主线路加多辅助线路。
- UI 支持启动/停止辅助端口和复制代理地址。
- Clash API 和流量统计识别辅助出站。
- 右键“解析为 IP”支持选择主线路或具体辅助线路作为 DoH 请求出站路径。

阶段 4：无偷跑重启。

- 区分用户显式关闭和配置重载/软件重启。
- 系统代理重启期间保持 fail-closed。
- 内部 TUN 重启路径设计 kill-switch 或热更新策略。
- 增加 Windows 本机验证脚本，检查重启期间系统代理注册表和 TUN 路由没有被错误撤销。

## 验收建议

导出验收：

- 从 Clash 订阅导入后，选中多条线路导出到剪贴板，JSON 中包含 `format`、`version`、`groups`、`defaults`、`items`。
- group 中 `source_type` 为 `clash`，`defaults.client.value` 为 `mihomo/1.19.28`，即使导出的线路并非 AnyTLS。
- AnyTLS 项包含协议字段、TLS 字段、idle-session 字段，并默认 `inherit.client=true`。
- 来自 Clash 的订阅 DoH 出现在 `defaults.server_resolver.doh_nameservers` 和兼容字段 `doh_nameservers`。
- 导出的精简包不包含 Clash `proxy-groups`、`rules`、dashboard、health-check。

MultiMapper 配合验收：

- MultiMapper 能从剪贴板识别精简包。
- 导入后 `doh_nameservers` 按 `source_tag` 落入 group metadata；后续支持 `defaults` 后，应优先读取 `defaults.server_resolver`。
- RouteFluent sing-box 生成配置时，域名 server 的 outbound 带 `domain_resolver`。
- Clash 来源 AnyTLS outbound 的 `client` 为 group 默认 `mihomo/1.19.28` 或用户指定 custom；非 AnyTLS 不写无效 client 字段。

解析验收：

- 同一线路可分别使用订阅 DoH、公共 DoH、系统 DNS 解析，并并列显示结果差异。
- 通过代理解析时，必须能选择主线路或某个辅助线路，结果中显示线路名称和端口。
- 没有运行线路时，选择“通过代理”会明确报错或提示先启动线路，不会静默回退。
- 解析完成后仍只允许用户显式选择替换、复制新增或复制结果。

多线路验收：

- 主线路启动后，Windows 系统代理指向主端口。
- 启动辅助线路后，主端口不变，辅助端口可单独 curl。
- 停止辅助线路不影响主线路系统代理和 TUN。
- 切换主线路时，辅助线路策略符合 UI 选择，且不会隐式清空系统代理。

无偷跑验收：

- 执行普通 profile restart 时，Windows 系统代理没有被清空。
- 执行软件自重启或管理员提权重启时，系统代理不被恢复为直连。
- 内部 TUN 重启期间要么不停 TUN 热更新，要么启用 fail-closed 保护；不得存在默认出口直连窗口。
- 只有用户明确点击关闭系统代理或关闭 TUN 时，才允许撤销对应 OS 状态。
