#include "db/ProfileFilter.hpp"
#include "fmt/includes.h"
#include "fmt/Preset.hpp"
#include "main/ConfigMutation.hpp"
#include "main/HTTPRequestHelper.hpp"

#include "GroupUpdater.hpp"

#include <QInputDialog>
#include <QCoreApplication>
#include <QMap>
#include <QSet>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#ifndef NKR_NO_YAML

#include <yaml-cpp/yaml.h>

#endif

namespace NekoGui_sub {

    GroupUpdater* groupUpdater = new GroupUpdater;

    struct GroupCommitSnapshot {
        qint64 subLastUpdate = 0;
        QString info;
        QString sourceType;
        QString defaultClientMode;
        QString defaultClientValue;
        QString defaultClientSource;
        QString defaultResolverDoh;
        QString defaultResolverSource;
        bool defaultResolverFallback = false;
        QList<int> order;

        explicit GroupCommitSnapshot(const std::shared_ptr<NekoGui::Group>& group) {
            if (group == nullptr) return;
            subLastUpdate = group->sub_last_update;
            info = group->info;
            sourceType = group->source_type;
            defaultClientMode = group->default_client_mode;
            defaultClientValue = group->default_client_value;
            defaultClientSource = group->default_client_source;
            defaultResolverDoh = group->default_server_resolver_doh;
            defaultResolverSource = group->default_server_resolver_source;
            defaultResolverFallback = group->default_server_resolver_allow_local_fallback;
            order = group->order;
        }

        void Restore(const std::shared_ptr<NekoGui::Group>& group) const {
            if (group == nullptr) return;
            group->sub_last_update = subLastUpdate;
            group->info = info;
            group->source_type = sourceType;
            group->default_client_mode = defaultClientMode;
            group->default_client_value = defaultClientValue;
            group->default_client_source = defaultClientSource;
            group->default_server_resolver_doh = defaultResolverDoh;
            group->default_server_resolver_source = defaultResolverSource;
            group->default_server_resolver_allow_local_fallback = defaultResolverFallback;
            group->order = order;
        }
    };

    void RawUpdater_FixEnt(const std::shared_ptr<NekoGui::ProxyEntity>& ent) {
        if (ent == nullptr) return;
        auto stream = NekoGui_fmt::GetStreamSettings(ent->bean.get());
        if (stream == nullptr) return;
        // 1. "security"
        if (stream->security == "none" || stream->security == "0" || stream->security == "false") {
            stream->security = "";
        } else if (stream->security == "1" || stream->security == "true") {
            stream->security = "tls";
        }
        // 2. TLS SNI: some legacy link builders encode SNI through host, so normalize it here.
        if (stream->security == "tls" && IsIpAddress(ent->bean->serverAddress) && (!stream->host.isEmpty()) && stream->sni.isEmpty()) {
            stream->sni = stream->host;
        }
    }

    void RawUpdater::update(const QString& str) {
        // Base64 encoded subscription
        if (auto str2 = DecodeB64IfValid(str); !str2.isEmpty()) {
            update(str2);
            return;
        }

        // Clash
        if (str.contains("proxies:")) {
            updateClash(str);
            return;
        }

        // Multi line
        if (str.count("\n") > 0) {
            auto list = str.split("\n");
            for (const auto& str2: list) {
                update(str2.trimmed());
            }
            return;
        }

        std::shared_ptr<NekoGui::ProxyEntity> ent;
        bool needFix = true;

        // Nekoray format
        if (str.startsWith("nekoray://")) {
            needFix = false;
            auto link = QUrl(str);
            if (!link.isValid()) return;
            ent = NekoGui::ProfileManager::NewProxyEntity(link.host());
            if (ent->bean->version == -114514) return;
            auto j = DecodeB64IfValid(link.fragment().toUtf8(), QByteArray::Base64UrlEncoding);
            if (j.isEmpty()) return;
            ent->bean->FromJsonBytes(j);
        }

        // SOCKS
        if (str.startsWith("socks5://") || str.startsWith("socks4://") ||
            str.startsWith("socks4a://") || str.startsWith("socks://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("socks");
            auto ok = ent->SocksHTTPBean()->TryParseLink(str);
            if (!ok) return;
        }

        // HTTP
        if (str.startsWith("http://") || str.startsWith("https://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("http");
            auto ok = ent->SocksHTTPBean()->TryParseLink(str);
            if (!ok) return;
        }

        // ShadowSocks
        if (str.startsWith("ss://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("shadowsocks");
            auto ok = ent->ShadowSocksBean()->TryParseLink(str);
            if (!ok) return;
        }

        // VMess
        if (str.startsWith("vmess://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("vmess");
            auto ok = ent->VMessBean()->TryParseLink(str);
            if (!ok) return;
        }

        // VLESS
        if (str.startsWith("vless://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("vless");
            auto ok = ent->TrojanVLESSBean()->TryParseLink(str);
            if (!ok) return;
        }

        // Trojan
        if (str.startsWith("trojan://")) {
            ent = NekoGui::ProfileManager::NewProxyEntity("trojan");
            auto ok = ent->TrojanVLESSBean()->TryParseLink(str);
            if (!ok) return;
        }

        // Naive was removed by the previous out-of-scope core convergence.
        // Keep this explicit unsupported branch until the NekoRay capability
        // is restored; it is not a product decision to drop the protocol.
        if (str.startsWith("naive+")) {
            return;
        }

        // Hysteria2
        if (str.startsWith("hysteria2://") || str.startsWith("hy2://")) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("hysteria2");
            auto ok = ent->QUICBean()->TryParseLink(str);
            if (!ok) return;
        }

        // TUIC
        if (str.startsWith("tuic://")) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("tuic");
            auto ok = ent->QUICBean()->TryParseLink(str);
            if (!ok) return;
        }

        // AnyTLS
        if (str.startsWith("anytls://")) {
            needFix = false;
            ent = NekoGui::ProfileManager::NewProxyEntity("anytls");
            auto ok = ent->AnyTLSBean()->TryParseLink(str);
            if (!ok) return;
        }

        if (ent == nullptr) return;

        // Fix
        if (needFix) RawUpdater_FixEnt(ent);

        // Stage only. GroupUpdater commits after the complete input is valid.
        updated_order += ent;
    }

#ifndef NKR_NO_YAML

    QString Node2QString(const YAML::Node& n, const QString& def = "") {
        try {
            return n.as<std::string>().c_str();
        } catch (const YAML::Exception& ex) {
            qDebug() << ex.what();
            return def;
        }
    }

    QStringList Node2QStringList(const YAML::Node& n) {
        try {
            if (n.IsSequence()) {
                QStringList list;
                for (auto item: n) {
                    list << item.as<std::string>().c_str();
                }
                return list;
            } else if (n.IsScalar()) {
                return {Node2QString(n)};
            } else {
                return {};
            }
        } catch (const YAML::Exception& ex) {
            qDebug() << ex.what();
            return {};
        }
    }

    int Node2Int(const YAML::Node& n, const int& def = 0) {
        try {
            return n.as<int>();
        } catch (const YAML::Exception& ex) {
            qDebug() << ex.what();
            return def;
        }
    }

    bool Node2Bool(const YAML::Node& n, const bool& def = false) {
        try {
            return n.as<bool>();
        } catch (const YAML::Exception& ex) {
            try {
                return n.as<int>();
            } catch (const YAML::Exception& ex2) {
                qDebug() << ex2.what();
            }
            qDebug() << ex.what();
            return def;
        }
    }

    QString Node2DurationSeconds(const YAML::Node& n) {
        if (!n.IsDefined()) return {};

        auto duration = Node2QString(n).trimmed();
        if (duration.isEmpty()) {
            auto seconds = Node2Int(n);
            if (seconds > 0) duration = Int2String(seconds);
        }

        bool ok = false;
        duration.toLongLong(&ok);
        if (ok && !duration.isEmpty()) duration += "s";
        return duration;
    }

    // NodeChild returns the first defined children or Null Node
    YAML::Node NodeChild(const YAML::Node& n, const std::list<std::string>& keys) {
        for (const auto& key: keys) {
            auto child = n[key];
            if (child.IsDefined()) return child;
        }
        return {};
    }

    bool IsValidClashDohUpstream(const QString& raw) {
        const auto url = QUrl(raw.trimmed());
        if (!url.isValid() || url.scheme().toLower() != "https") return false;
        if (url.host().isEmpty()) return false;
        if (url.path().isEmpty() || url.path() == "/") return false;
        if (!url.userName().isEmpty() || !url.password().isEmpty()) return false;
        if (url.hasQuery() || url.hasFragment()) return false;
        return true;
    }

    struct ClashDohUpstreams {
        bool present = false;
        QStringList valid;
        QStringList invalid;
    };

    ClashDohUpstreams ExtractClashProxyServerDohUpstreams(const YAML::Node& root) {
        ClashDohUpstreams result;
        auto dns = root["dns"];
        if (!dns.IsMap()) return result;

        const auto configured = NodeChild(dns, {
                                                   "proxy-server-nameserver",
                                                   "proxy_server_nameserver",
                                               });
        if (!configured.IsDefined()) return result;
        result.present = true;

        QSet<QString> seen;
        for (const auto& raw: Node2QStringList(configured)) {
            const auto value = raw.trimmed();
            if (value.isEmpty() || seen.contains(value)) continue;
            seen.insert(value);
            if (IsValidClashDohUpstream(value)) {
                result.valid << value;
            } else {
                result.invalid << value;
            }
        }
        return result;
    }

    bool IsVisibleAsciiAnyTLSClientValue(const QString& value) {
        if (value.isEmpty() || value.size() > 128) return false;
        for (auto ch: value) {
            const auto code = ch.unicode();
            if (code < 0x21 || code > 0x7e) return false;
        }
        return true;
    }

#endif

    // https://github.com/Dreamacro/clash/wiki/configuration
    void RawUpdater::updateClash(const QString& str) {
#ifndef NKR_NO_YAML
        try {
            auto root = YAML::Load(str.toStdString());
            if (!root.IsMap()) {
                parse_failed = true;
                parse_error = QObject::tr("The Clash subscription root is not a map.");
                return;
            }

            auto proxies = root["proxies"];
            if (!proxies.IsSequence()) {
                parse_failed = true;
                parse_error = QObject::tr("The Clash subscription does not contain a valid proxies list.");
                return;
            }

            const auto clashDohResult = ExtractClashProxyServerDohUpstreams(root);
            if (clashDohResult.present && clashDohResult.valid.isEmpty()) {
                parse_failed = true;
                parse_error = QObject::tr(
                    "dns.proxy-server-nameserver is present but contains no valid HTTPS DoH endpoint.");
                return;
            }
            const auto clashDohUpstreams = clashDohResult.valid;
            detected_source_type = "clash";
            detected_doh_upstreams = clashDohUpstreams;
            detected_invalid_doh_upstreams = clashDohResult.invalid;
            for (auto proxy: proxies) {
                if (!proxy.IsMap()) continue;
                auto type = Node2QString(proxy["type"]).toLower();
                auto type_clash = type;

                if (type == "ss" || type == "ssr") type = "shadowsocks";
                if (type == "socks5") type = "socks";
                if (type == "naive") continue;

                auto ent = NekoGui::ProfileManager::NewProxyEntity(type);
                if (ent->bean->version == -114514) continue;
                bool needFix = false;

                // common
                ent->bean->name = Node2QString(proxy["name"]);
                ent->bean->serverAddress = Node2QString(proxy["server"]);
                ent->bean->serverPort = Node2Int(proxy["port"]);
                // The supported Clash extension is only the top-level
                // dns.proxy-server-nameserver field. Do not accept the prior
                // private per-proxy server-resolver schema or its local
                // fallback switch.
                if (!clashDohUpstreams.isEmpty() &&
                    !ent->bean->serverAddress.isEmpty() && !IsIpAddress(ent->bean->serverAddress)) {
                    ent->bean->inheritSubscriptionResolver = true;
                    ent->bean->serverResolverDohUpstreams = "";
                    ent->bean->serverResolverAllowLocalFallback = false;
                }

                if (type_clash == "ss") {
                    auto bean = ent->ShadowSocksBean();
                    bean->method = Node2QString(proxy["cipher"]).replace("dummy", "none");
                    bean->password = Node2QString(proxy["password"]);
                    auto plugin_n = proxy["plugin"];
                    auto pluginOpts_n = proxy["plugin-opts"];

                    // UDP over TCP
                    if (Node2Bool(proxy["udp-over-tcp"])) {
                        bean->uot = Node2Int(proxy["udp-over-tcp-version"]);
                        if (bean->uot == 0) bean->uot = 2;
                    }

                    if (plugin_n.IsDefined() && pluginOpts_n.IsDefined()) {
                        QStringList ssPlugin;
                        auto plugin = Node2QString(plugin_n);
                        if (plugin == "obfs") {
                            ssPlugin << "obfs-local";
                            ssPlugin << "obfs=" + Node2QString(pluginOpts_n["mode"]);
                            ssPlugin << "obfs-host=" + Node2QString(pluginOpts_n["host"]);
                        }
                        bean->plugin = ssPlugin.join(";");
                    }

                    // sing-mux
                    auto smux = NodeChild(proxy, {"smux"});
                    if (Node2Bool(smux["enabled"])) bean->stream->multiplex_status = 1;
                } else if (type == "socks" || type == "http") {
                    auto bean = ent->SocksHTTPBean();
                    bean->username = Node2QString(proxy["username"]);
                    bean->password = Node2QString(proxy["password"]);
                    if (Node2Bool(proxy["tls"])) bean->stream->security = "tls";
                    if (Node2Bool(proxy["skip-cert-verify"])) bean->stream->allow_insecure = true;
                } else if (type == "trojan" || type == "vless") {
                    needFix = true;
                    auto bean = ent->TrojanVLESSBean();
                    if (type == "vless") {
                        bean->flow = Node2QString(proxy["flow"]);
                        bean->password = Node2QString(proxy["uuid"]);
                        // meta packet encoding
                        if (Node2Bool(proxy["packet-addr"])) {
                            bean->stream->packet_encoding = "packetaddr";
                        } else {
                            // For VLESS, default to use xudp
                            bean->stream->packet_encoding = "xudp";
                        }
                    } else {
                        bean->password = Node2QString(proxy["password"]);
                    }
                    bean->stream->security = "tls";
                    bean->stream->network = Node2QString(proxy["network"], "tcp");
                    bean->stream->sni = FIRST_OR_SECOND(Node2QString(proxy["sni"]), Node2QString(proxy["servername"]));
                    bean->stream->alpn = Node2QStringList(proxy["alpn"]).join(",");
                    bean->stream->allow_insecure = Node2Bool(proxy["skip-cert-verify"]);
                    bean->stream->utlsFingerprint = Node2QString(proxy["client-fingerprint"]);
                    if (bean->stream->utlsFingerprint.isEmpty()) {
                        bean->stream->utlsFingerprint = fallback_utls_fingerprint;
                    }

                    // sing-mux
                    auto smux = NodeChild(proxy, {"smux"});
                    if (Node2Bool(smux["enabled"])) bean->stream->multiplex_status = 1;

                    // opts
                    auto ws = NodeChild(proxy, {"ws-opts", "ws-opt"});
                    if (ws.IsMap()) {
                        auto headers = ws["headers"];
                        for (auto header: headers) {
                            if (Node2QString(header.first).toLower() == "host") {
                                bean->stream->host = Node2QString(header.second);
                            }
                        }
                        bean->stream->path = Node2QString(ws["path"]);
                        bean->stream->ws_early_data_length = Node2Int(ws["max-early-data"]);
                        bean->stream->ws_early_data_name = Node2QString(ws["early-data-header-name"]);
                    }

                    auto grpc = NodeChild(proxy, {"grpc-opts", "grpc-opt"});
                    if (grpc.IsMap()) {
                        bean->stream->path = Node2QString(grpc["grpc-service-name"]);
                    }

                    auto reality = NodeChild(proxy, {"reality-opts"});
                    if (reality.IsMap()) {
                        bean->stream->reality_pbk = Node2QString(reality["public-key"]);
                        bean->stream->reality_sid = Node2QString(reality["short-id"]);
                    }
                } else if (type == "vmess") {
                    needFix = true;
                    auto bean = ent->VMessBean();
                    bean->uuid = Node2QString(proxy["uuid"]);
                    bean->aid = Node2Int(proxy["alterId"]);
                    bean->security = Node2QString(proxy["cipher"], bean->security);
                    bean->stream->network = Node2QString(proxy["network"], "tcp").replace("h2", "http");
                    bean->stream->sni = FIRST_OR_SECOND(Node2QString(proxy["sni"]), Node2QString(proxy["servername"]));
                    bean->stream->alpn = Node2QStringList(proxy["alpn"]).join(",");
                    if (Node2Bool(proxy["tls"])) bean->stream->security = "tls";
                    if (Node2Bool(proxy["skip-cert-verify"])) bean->stream->allow_insecure = true;
                    bean->stream->utlsFingerprint = Node2QString(proxy["client-fingerprint"]);
                    bean->stream->utlsFingerprint = Node2QString(proxy["client-fingerprint"]);
                    if (bean->stream->utlsFingerprint.isEmpty()) {
                        bean->stream->utlsFingerprint = fallback_utls_fingerprint;
                    }

                    // sing-mux
                    auto smux = NodeChild(proxy, {"smux"});
                    if (Node2Bool(smux["enabled"])) bean->stream->multiplex_status = 1;

                    // meta packet encoding
                    if (Node2Bool(proxy["xudp"])) bean->stream->packet_encoding = "xudp";
                    if (Node2Bool(proxy["packet-addr"])) bean->stream->packet_encoding = "packetaddr";

                    // opts
                    auto ws = NodeChild(proxy, {"ws-opts", "ws-opt"});
                    if (ws.IsMap()) {
                        auto headers = ws["headers"];
                        for (auto header: headers) {
                            if (Node2QString(header.first).toLower() == "host") {
                                bean->stream->host = Node2QString(header.second);
                            }
                        }
                        bean->stream->path = Node2QString(ws["path"]);
                        bean->stream->ws_early_data_length = Node2Int(ws["max-early-data"]);
                        bean->stream->ws_early_data_name = Node2QString(ws["early-data-header-name"]);
                        // Legacy WebSocket early data compatibility.
                        if (Node2QString(ws["early-data-header-name"]) == "Sec-WebSocket-Protocol") {
                            bean->stream->path += "?ed=" + Node2QString(ws["max-early-data"]);
                        }
                    }

                    auto grpc = NodeChild(proxy, {"grpc-opts", "grpc-opt"});
                    if (grpc.IsMap()) {
                        bean->stream->path = Node2QString(grpc["grpc-service-name"]);
                    }

                    auto h2 = NodeChild(proxy, {"h2-opts", "h2-opt"});
                    if (h2.IsMap()) {
                        auto hosts = h2["host"];
                        for (auto host: hosts) {
                            bean->stream->host = Node2QString(host);
                            break;
                        }
                        bean->stream->path = Node2QString(h2["path"]);
                    }

                    auto tcp_http = NodeChild(proxy, {"http-opts", "http-opt"});
                    if (tcp_http.IsMap()) {
                        bean->stream->network = "tcp";
                        bean->stream->header_type = "http";
                        auto headers = tcp_http["headers"];
                        for (auto header: headers) {
                            if (Node2QString(header.first).toLower() == "host") {
                                bean->stream->host = Node2QString(header.second[0]);
                            }
                            break;
                        }
                        auto paths = tcp_http["path"];
                        for (auto path: paths) {
                            bean->stream->path = Node2QString(path);
                            break;
                        }
                    }
                } else if (type == "hysteria2") {
                    auto bean = ent->QUICBean();

                    bean->hopPort = Node2QString(proxy["ports"]);

                    bean->allowInsecure = Node2Bool(proxy["skip-cert-verify"]);
                    bean->caText = Node2QString(proxy["ca-str"]);
                    bean->sni = Node2QString(proxy["sni"]);

                    bean->obfsPassword = Node2QString(proxy["obfs-password"]);
                    bean->password = Node2QString(proxy["password"]);

                    bean->uploadMbps = Node2QString(proxy["up"]).split(" ")[0].toInt();
                    bean->downloadMbps = Node2QString(proxy["down"]).split(" ")[0].toInt();
                } else if (type == "tuic") {
                    auto bean = ent->QUICBean();

                    bean->uuid = Node2QString(proxy["uuid"]);
                    bean->password = Node2QString(proxy["password"]);

                    if (Node2Int(proxy["heartbeat-interval"]) != 0) {
                        bean->heartbeat = Int2String(Node2Int(proxy["heartbeat-interval"])) + "ms";
                    }

                    bean->udpRelayMode = Node2QString(proxy["udp-relay-mode"], bean->udpRelayMode);
                    bean->congestionControl = Node2QString(proxy["congestion-controller"], bean->congestionControl);

                    bean->disableSni = Node2Bool(proxy["disable-sni"]);
                    bean->zeroRttHandshake = Node2Bool(proxy["reduce-rtt"]);
                    bean->allowInsecure = Node2Bool(proxy["skip-cert-verify"]);
                    bean->alpn = Node2QStringList(proxy["alpn"]).join(",");
                    bean->caText = Node2QString(proxy["ca-str"]);
                    bean->sni = Node2QString(proxy["sni"]);

                    if (Node2Bool(proxy["udp-over-stream"])) bean->uos = true;

                    if (!Node2QString(proxy["ip"]).isEmpty()) {
                        if (bean->sni.isEmpty()) bean->sni = bean->serverAddress;
                        bean->serverAddress = Node2QString(proxy["ip"]);
                    }
                } else if (type == "anytls") {
                    auto bean = ent->AnyTLSBean();

                    bean->password = Node2QString(proxy["password"]);
                    bean->allowInsecure = Node2Bool(proxy["skip-cert-verify"]) || Node2Bool(proxy["insecure"]);
                    bean->disableSni = Node2Bool(proxy["disable-sni"]) || Node2Bool(proxy["disable_sni"]);
                    bean->certificate = FIRST_OR_SECOND(Node2QString(proxy["ca-str"]),
                                                        Node2QString(proxy["certificate"]));
                    bean->sni = FIRST_OR_SECOND(Node2QString(proxy["sni"]),
                                                FIRST_OR_SECOND(Node2QString(proxy["servername"]),
                                                                Node2QString(proxy["server_name"])));
                    bean->alpn = Node2QStringList(proxy["alpn"]).join(",");
                    bean->utlsFingerprint = FIRST_OR_SECOND(Node2QString(proxy["client-fingerprint"]),
                                                            FIRST_OR_SECOND(Node2QString(proxy["fingerprint"]),
                                                                            Node2QString(proxy["utls_fingerprint"])));

                    bean->idleSessionCheckInterval = FIRST_OR_SECOND(Node2DurationSeconds(proxy["idle-session-check-interval"]),
                                                                     Node2DurationSeconds(proxy["idle_session_check_interval"]));
                    bean->idleSessionTimeout = FIRST_OR_SECOND(Node2DurationSeconds(proxy["idle-session-timeout"]),
                                                               Node2DurationSeconds(proxy["idle_session_timeout"]));
                    bean->minIdleSession = proxy["min-idle-session"].IsDefined()
                                               ? Node2Int(proxy["min-idle-session"])
                                               : Node2Int(proxy["min_idle_session"]);
                    bean->anytlsClientMode = FIRST_OR_SECOND(Node2QString(proxy["anytls-client-mode"]),
                                                             Node2QString(proxy["anytls_client_mode"]))
                                                 .trimmed()
                                                 .toLower();
                    bean->anytlsClientValue = FIRST_OR_SECOND(Node2QString(proxy["anytls-client-value"]),
                                                              Node2QString(proxy["anytls_client_value"]))
                                                  .trimmed();
                    const auto clashAnyTLSClient = Node2QString(proxy["client"]).trimmed();
                    auto hasExplicitAnyTLSClient = !bean->anytlsClientMode.isEmpty() || !clashAnyTLSClient.isEmpty();
                    if (bean->anytlsClientMode.isEmpty() && !clashAnyTLSClient.isEmpty()) {
                        if (clashAnyTLSClient == "mihomo/1.19.28") {
                            bean->anytlsClientMode = "mihomo";
                        } else if (IsVisibleAsciiAnyTLSClientValue(clashAnyTLSClient)) {
                            bean->anytlsClientMode = "custom";
                            bean->anytlsClientValue = clashAnyTLSClient;
                        }
                    }
                    if (bean->anytlsClientMode.isEmpty()) {
                        // Clash subscriptions use the group-level Mihomo client default unless a node overrides it.
                        bean->anytlsClientMode = "native";
                    }
                    ent->bean->inheritSubscriptionClient = !hasExplicitAnyTLSClient;
                    if (bean->anytlsClientMode == "custom" && bean->anytlsClientValue.isEmpty() &&
                        IsVisibleAsciiAnyTLSClientValue(clashAnyTLSClient)) {
                        bean->anytlsClientValue = clashAnyTLSClient;
                    }
                    if (bean->anytlsClientMode != "custom") bean->anytlsClientValue = "";

                    auto reality = NodeChild(proxy, {"reality-opts", "reality"});
                    if (reality.IsMap()) {
                        bean->realityPublicKey = FIRST_OR_SECOND(Node2QString(reality["public-key"]),
                                                                 Node2QString(reality["public_key"]));
                        bean->realityShortId = FIRST_OR_SECOND(Node2QString(reality["short-id"]),
                                                               Node2QString(reality["short_id"]));
                    }
                } else {
                    continue;
                }

                // All Clash proxy types supported here require a concrete
                // network endpoint. An empty entry must not make an otherwise
                // invalid response destructive.
                if (ent->bean->serverAddress.trimmed().isEmpty() ||
                    ent->bean->serverPort < 1 || ent->bean->serverPort > 65535) {
                    continue;
                }

                if (needFix) RawUpdater_FixEnt(ent);
                updated_order += ent;
            }
        } catch (const YAML::Exception& ex) {
            parse_failed = true;
            parse_error = QString::fromUtf8(ex.what());
            runOnUiThread([=] {
                MessageBoxWarning("YAML Exception", ex.what());
            });
        } catch (const std::exception& ex) {
            parse_failed = true;
            parse_error = QString::fromUtf8(ex.what());
        } catch (...) {
            parse_failed = true;
            parse_error = QObject::tr("Unknown Clash subscription parsing error.");
        }
#else
        Q_UNUSED(str);
        parse_failed = true;
        parse_error = QObject::tr("This build does not support Clash YAML subscriptions.");
#endif
    }

    // 在新的 thread 运行
    void GroupUpdater::AsyncUpdate(const QString& str, int _sub_gid, const std::function<void()>& finish) {
        auto content = str.trimmed();
        bool asURL = false;
        bool createNewGroup = false;

        if (_sub_gid < 0 && (content.startsWith("http://") || content.startsWith("https://"))) {
            auto items = QStringList{
                QObject::tr("As Subscription (add to this group)"),
                QObject::tr("As Subscription (create new group)"),
                QObject::tr("As link"),
            };
            bool ok;
            auto a = QInputDialog::getItem(nullptr,
                                           QObject::tr("url detected"),
                                           QObject::tr("%1\nHow to update?").arg(content),
                                           items, 0, false, &ok);
            if (!ok) return;
            if (items.indexOf(a) <= 1) asURL = true;
            if (items.indexOf(a) == 1) createNewGroup = true;
        }

        runOnNewThread([=] {
            const auto gid = Update(str, _sub_gid, asURL, createNewGroup);
            runOnUiThread([=] {
                emit asyncUpdateCallback(gid);
                if (finish != nullptr) finish();
            });
        });
    }

    int GroupUpdater::Update(const QString& _str, int _sub_gid, bool _not_sub_as_url,
                             bool _create_new_group) {
        // 创建 rawUpdater
        auto rawUpdater = std::make_unique<RawUpdater>();
        rawUpdater->gid_add_to = _sub_gid;

        // 准备
        QString sub_user_info;
        bool asURL = _sub_gid >= 0 || _not_sub_as_url; // 把 _str 当作 url 处理（下载内容）
        auto content = _str.trimmed();
        std::shared_ptr<NekoGui::Group> group;
        std::shared_ptr<NekoGui::Group> requestedGroup;
        int requestedGroupId = -1;
        QByteArray requestedGroupState;
        QMap<int, std::shared_ptr<NekoGui::ProxyEntity>> requestedGroupMembers;
        QMap<int, QByteArray> requestedGroupMemberStates;
        QString requestedGroupName;
        NekoGui_network::NekoHTTPRequestOptions requestOptions;
        bool clearExistingProfiles = false;
        const bool updatesExistingSubscriptionGroup = _sub_gid >= 0 && !_create_new_group;
        auto captureRequestedState = [&]() {
            NekoGui_ConfigMutation::Guard snapshotGuard(true);
            if (NekoGui::dataStore->prepare_exit || QCoreApplication::closingDown()) return false;
            if (NekoGui::dataStore->core_transition_depth.load() > 0) return false;
            NekoGui::dataStore->imported_count = 0;
            rawUpdater->fallback_utls_fingerprint = NekoGui::dataStore->utlsFingerprint;
            clearExistingProfiles = NekoGui::dataStore->sub_clear;
            requestOptions.useProxy = NekoGui::dataStore->sub_use_proxy;
            requestOptions.proxyAvailable = NekoGui::dataStore->started_id >= 0;
            requestOptions.proxyPort = NekoGui::dataStore->inbound_socks_port;
            if (NekoGui::dataStore->inbound_auth->NeedAuth()) {
                requestOptions.proxyUsername = NekoGui::dataStore->inbound_auth->username;
                requestOptions.proxyPassword = NekoGui::dataStore->inbound_auth->password;
            }
            requestOptions.userAgent = NekoGui::dataStore->GetUserAgent();
            requestOptions.insecureTls = NekoGui::dataStore->sub_insecure;

            if (!_create_new_group) {
                requestedGroupId = _sub_gid >= 0
                                       ? _sub_gid
                                       : NekoGui::dataStore->current_group;
                requestedGroup = NekoGui::profileManager->GetGroup(requestedGroupId);
                if (requestedGroup == nullptr || requestedGroup->archive ||
                    requestedGroup->save_control_no_save) {
                    return false;
                }
                requestedGroupState = requestedGroup->ToJsonBytes();
                requestedGroupMembers.clear();
                requestedGroupMemberStates.clear();
                for (const auto& profile: requestedGroup->Profiles()) {
                    if (profile == nullptr || profile->id < 0 ||
                        profile->save_control_no_save || profile->gid != requestedGroupId ||
                        requestedGroupMembers.contains(profile->id)) {
                        return false;
                    }
                    requestedGroupMembers.insert(profile->id, profile);
                    requestedGroupMemberStates.insert(profile->id, profile->ToJsonBytes());
                }
                requestedGroupName = requestedGroup->name;
                rawUpdater->gid_add_to = requestedGroupId;
                if (updatesExistingSubscriptionGroup) group = requestedGroup;
            }
            return true;
        };
        bool targetEligible = false;
        auto* application = QCoreApplication::instance();
        if (application != nullptr && QThread::currentThread() != application->thread()) {
            const auto invoked = QMetaObject::invokeMethod(
                application,
                [&] { targetEligible = captureRequestedState(); },
                Qt::BlockingQueuedConnection);
            if (!invoked) return _sub_gid;
        } else {
            targetEligible = captureRequestedState();
        }
        if (!targetEligible) return _sub_gid;

        // 网络请求
        if (asURL) {
            auto groupName = requestedGroup == nullptr ? content : requestedGroupName;
            MW_show_log(">>>>>>>> " + QObject::tr("Requesting subscription: %1").arg(groupName));

            auto resp = NetworkRequestHelper::HttpGet(content, requestOptions);
            if (!resp.error.isEmpty()) {
                // The response body may be an HTML login page, a subscription
                // payload, or contain credentials. Never copy it into logs.
                MW_show_log("<<<<<<<< " + QObject::tr("Requesting subscription %1 error: %2").arg(groupName, resp.error));
                return _sub_gid;
            }

            content = resp.data;
            sub_user_info = NetworkRequestHelper::GetHeader(resp.header, "Subscription-UserInfo");

            MW_show_log("<<<<<<<< " + QObject::tr("Subscription request fininshed: %1").arg(groupName));
        }

        QList<std::shared_ptr<NekoGui::ProxyEntity>> in;          // 更新前
        QList<std::shared_ptr<NekoGui::ProxyEntity>> out_all;     // 更新前 + 更新后
        QList<std::shared_ptr<NekoGui::ProxyEntity>> out;         // 更新后
        QList<std::shared_ptr<NekoGui::ProxyEntity>> only_in;     // 只在更新前有的
        QList<std::shared_ptr<NekoGui::ProxyEntity>> only_out;    // 只在更新后有的
        QList<std::shared_ptr<NekoGui::ProxyEntity>> update_del;  // 更新前后都有的，需要删除的新配置
        QList<std::shared_ptr<NekoGui::ProxyEntity>> update_keep; // 更新前后都有的，被保留的旧配置

        // Parse the complete response without touching groups or profile files.
        rawUpdater->update(content);

        if (rawUpdater->parse_failed || rawUpdater->updated_order.isEmpty()) {
            auto reason = rawUpdater->parse_failed
                              ? rawUpdater->parse_error
                              : QObject::tr("No supported profiles were found.");
            if (reason.isEmpty()) reason = QObject::tr("Subscription parsing failed.");
            MW_show_log("<<<<<<<< " + QObject::tr("Subscription update aborted without changes: %1").arg(reason));
            return _sub_gid;
        }
        if (!rawUpdater->detected_invalid_doh_upstreams.isEmpty()) {
            MW_show_log(QObject::tr(
                            "Ignored %1 unsupported proxy-server-nameserver item(s); valid HTTPS DoH endpoints remain active.")
                            .arg(rawUpdater->detected_invalid_doh_upstreams.size()));
        }

        // Downloads and parsing stay off the UI thread. The complete legacy
        // commit is then dispatched to the UI thread and serialized with all
        // guarded background saves, preventing UI model edits from racing the
        // profile/group containers while this transitional path is retained.
        auto commitParsedUpdate = [&]() -> int {
            NekoGui_ConfigMutation::Guard mutationGuard(true);
            if (NekoGui::dataStore->prepare_exit || QCoreApplication::closingDown()) {
                return _sub_gid;
            }
            if (NekoGui::dataStore->core_transition_depth.load() > 0) {
                MW_show_log("<<<<<<<< " + QObject::tr(
                                              "Subscription update aborted without changes during a core transition."));
                return _sub_gid;
            }
            if (!_create_new_group) {
                const auto currentGroup = NekoGui::profileManager->GetGroup(requestedGroupId);
                if (currentGroup == nullptr || currentGroup != requestedGroup ||
                    currentGroup->save_control_no_save || currentGroup->archive ||
                    currentGroup->ToJsonBytes() != requestedGroupState) {
                    MW_show_log("<<<<<<<< " + QObject::tr(
                                                  "Subscription update aborted without changes because the target group changed while downloading."));
                    return _sub_gid;
                }
                const auto currentMembers = currentGroup->Profiles();
                bool membersUnchanged = currentMembers.size() == requestedGroupMembers.size();
                for (const auto& current: currentMembers) {
                    if (!membersUnchanged) break;
                    membersUnchanged = current != nullptr && current->id >= 0 &&
                                       requestedGroupMembers.value(current->id) == current &&
                                       current->gid == requestedGroupId &&
                                       !current->save_control_no_save &&
                                       requestedGroupMemberStates.value(current->id) ==
                                           current->ToJsonBytes();
                }
                if (!membersUnchanged) {
                    MW_show_log("<<<<<<<< " + QObject::tr(
                                                  "Subscription update aborted without changes because the target group's members changed while downloading."));
                    return _sub_gid;
                }
                rawUpdater->gid_add_to = requestedGroupId;
                if (updatesExistingSubscriptionGroup) group = currentGroup;
            }

            // Create a requested subscription group only after validation, so an
            // invalid response cannot leave an empty group behind.
            bool createdGroup = false;
            if (_create_new_group) {
                group = NekoGui::ProfileManager::NewGroup();
                group->name = QUrl(_str).host();
                group->url = _str;
                if (!NekoGui::profileManager->AddGroup(group)) {
                    MW_show_log("<<<<<<<< " + QObject::tr("Subscription update aborted: failed to create the group."));
                    return _sub_gid;
                }
                _sub_gid = group->id;
                rawUpdater->gid_add_to = _sub_gid;
                createdGroup = true;
            }

            if (group != nullptr) in = group->Profiles();
            const GroupCommitSnapshot groupSnapshot(group);

            // Commit all staged profiles before changing group metadata or deleting
            // old profiles. Roll back new insertions if one is rejected before the
            // group commit begins.
            QList<std::shared_ptr<NekoGui::ProxyEntity>> committed;
            bool rollbackIncomplete = false;
            auto rollbackCommitted = [&](const QString& reason) {
                bool complete = true;
                for (const auto& added: committed) {
                    QString deletionError;
                    if (!NekoGui::profileManager->DeleteProfile(added->id, reason, &deletionError)) {
                        complete = false;
                        qCritical() << "Subscription rollback preserved profile" << added->id << deletionError;
                    }
                }
                if (createdGroup) {
                    QString deletionError;
                    if (!NekoGui::profileManager->DeleteGroup(_sub_gid, reason, &deletionError)) {
                        complete = false;
                        qCritical() << "Subscription rollback preserved group" << _sub_gid << deletionError;
                    }
                }
                if (!complete) {
                    rollbackIncomplete = true;
                    MW_show_log("<<<<<<<< " + QObject::tr(
                                                  "Subscription rollback was incomplete; preserved files remain loaded for manual review."));
                }
                return complete;
            };
            for (const auto& ent: rawUpdater->updated_order) {
                if (!NekoGui::profileManager->AddProfile(ent, rawUpdater->gid_add_to)) {
                    rollbackCommitted(QStringLiteral("Rollback of an uncommitted subscription update."));
                    MW_show_log("<<<<<<<< " + QObject::tr("Subscription update aborted: failed to add all staged profiles."));
                    return createdGroup && !rollbackIncomplete ? -1 : _sub_gid;
                }
                committed += ent;
            }

            if (group != nullptr) {
                group->sub_last_update = QDateTime::currentMSecsSinceEpoch() / 1000;
                group->info = sub_user_info;
                if (rawUpdater->detected_source_type == "clash") {
                    group->source_type = "clash";
                    if (group->DefaultClientManagedBySubscription()) {
                        group->default_client_mode = "mihomo";
                        group->default_client_value = "mihomo/1.19.28";
                        group->SetDefaultClientManagedBySubscription(true);
                    }
                    if (group->DefaultResolverManagedBySubscription()) {
                        group->default_server_resolver_doh = rawUpdater->detected_doh_upstreams.join("\n");
                        group->default_server_resolver_allow_local_fallback = false;
                        group->SetDefaultResolverManagedBySubscription(true);
                    }
                } else if (group->source_type.isEmpty() && asURL) {
                    group->source_type = "subscription";
                }

                out_all = group->Profiles();

                auto saveGroupOrRollback = [&]() {
                    group->Save();
                    if (group->last_save_succeeded) return true;
                    if (group->last_save_indeterminate) {
                        MW_show_log("<<<<<<<< " + QObject::tr(
                                                      "Subscription update became indeterminate; new and old profiles were preserved for explicit recovery."));
                        return false;
                    }

                    groupSnapshot.Restore(group);
                    rollbackCommitted(QStringLiteral("Rollback after subscription group metadata save failed."));
                    MW_show_log("<<<<<<<< " + QObject::tr(
                                                  "Subscription update aborted: group metadata could not be committed; old profiles were preserved."));
                    return false;
                };

                QString change_text;
                int deletionFailures = 0;
                auto deleteOldProfile = [&](const std::shared_ptr<NekoGui::ProxyEntity>& profile,
                                            const QString& reason) {
                    QString deletionError;
                    if (NekoGui::profileManager->DeleteProfile(profile->id, reason, &deletionError)) return;
                    deletionFailures++;
                    qCritical() << "Subscription cleanup preserved profile" << profile->id << deletionError;
                };

                if (clearExistingProfiles) {
                    group->order.clear();
                    for (const auto& ent: rawUpdater->updated_order) {
                        group->order.append(ent->id);
                        change_text += "[+] " + ent->bean->DisplayTypeAndName() + "\n";
                    }
                    if (!saveGroupOrRollback()) return createdGroup ? -1 : _sub_gid;

                    MW_show_log(QObject::tr("Clearing servers..."));
                    for (const auto& profile: in) {
                        deleteOldProfile(
                            profile, QStringLiteral("Subscription clear removed an old profile."));
                    }
                } else {
                    // find and delete not updated profile by ProfileFilter
                    NekoGui::ProfileFilter::OnlyInSrc_ByPointer(out_all, in, out);
                    NekoGui::ProfileFilter::OnlyInSrc(in, out, only_in);
                    NekoGui::ProfileFilter::OnlyInSrc(out, in, only_out);
                    NekoGui::ProfileFilter::Common(in, out, update_keep, update_del, false);

                    QString notice_added;
                    QString notice_deleted;
                    for (const auto& ent: only_out) {
                        notice_added += "[+] " + ent->bean->DisplayTypeAndName() + "\n";
                    }
                    for (const auto& ent: only_in) {
                        notice_deleted += "[-] " + ent->bean->DisplayTypeAndName() + "\n";
                    }

                    // sort according to order in remote
                    group->order = {};
                    for (const auto& ent: rawUpdater->updated_order) {
                        auto deleted_index = update_del.indexOf(ent);
                        if (deleted_index >= 0) {
                            if (deleted_index >= update_keep.count()) continue; // should not happen
                            auto ent2 = update_keep[deleted_index];
                            group->order.append(ent2->id);
                        } else {
                            group->order.append(ent->id);
                        }
                    }
                    if (!saveGroupOrRollback()) return createdGroup ? -1 : _sub_gid;

                    // cleanup
                    for (const auto& ent: out_all) {
                        if (!group->order.contains(ent->id)) {
                            deleteOldProfile(
                                ent, QStringLiteral("Subscription refresh removed a superseded profile."));
                        }
                    }

                    change_text = "\n" + QObject::tr("Added %1 profiles:\n%2\nDeleted %3 Profiles:\n%4")
                                             .arg(only_out.length())
                                             .arg(notice_added)
                                             .arg(only_in.length())
                                             .arg(notice_deleted);
                    if (only_out.length() + only_in.length() == 0) change_text = QObject::tr("Nothing");
                }

                if (deletionFailures > 0) {
                    change_text += "\n" + QObject::tr(
                                              "%1 old profile(s) could not be deleted and were preserved for manual review.")
                                              .arg(deletionFailures);
                }

                MW_show_log("<<<<<<<< " + QObject::tr("Change of %1:").arg(group->name) + "\n" + change_text);
                if (createdGroup) MW_dialog_message("SubUpdater", "NewGroup");
                MW_dialog_message("SubUpdater", "finish-dingyue");
            } else {
                NekoGui::dataStore->imported_count = rawUpdater->updated_order.count();
                MW_dialog_message("SubUpdater", "finish");
            }
            return _sub_gid;
        };

        if (application == nullptr || QThread::currentThread() == application->thread()) {
            return commitParsedUpdate();
        }
        if (QCoreApplication::closingDown()) return _sub_gid;
        int commitResult = _sub_gid;
        const auto invoked = QMetaObject::invokeMethod(
            application,
            [&] { commitResult = commitParsedUpdate(); },
            Qt::BlockingQueuedConnection);
        if (!invoked) return _sub_gid;
        return commitResult;
    }
} // namespace NekoGui_sub

bool UI_update_all_groups_Updating = false;

#define should_skip_group(g) (g == nullptr || g->url.isEmpty() || g->archive || (onlyAllowed && g->skip_auto_update))

void serialUpdateSubscription(const QList<int>& groupsTabOrder, int _order, bool onlyAllowed) {
    if (_order >= groupsTabOrder.size()) {
        UI_update_all_groups_Updating = false;
        return;
    }

    // calculate this group
    auto group = NekoGui::profileManager->GetGroup(groupsTabOrder[_order]);
    if (group == nullptr || should_skip_group(group)) {
        serialUpdateSubscription(groupsTabOrder, _order + 1, onlyAllowed);
        return;
    }

    int nextOrder = _order + 1;
    while (nextOrder < groupsTabOrder.size()) {
        auto nextGid = groupsTabOrder[nextOrder];
        auto nextGroup = NekoGui::profileManager->GetGroup(nextGid);
        if (!should_skip_group(nextGroup)) {
            break;
        }
        nextOrder += 1;
    }

    // Async update current group
    UI_update_all_groups_Updating = true;
    NekoGui_sub::groupUpdater->AsyncUpdate(group->url, group->id, [=] {
        serialUpdateSubscription(groupsTabOrder, nextOrder, onlyAllowed);
    });
}

void UI_update_all_groups(bool onlyAllowed) {
    if (UI_update_all_groups_Updating) {
        MW_show_log("The last subscription update has not exited.");
        return;
    }

    auto groupsTabOrder = NekoGui::profileManager->groupsTabOrder;
    serialUpdateSubscription(groupsTabOrder, 0, onlyAllowed);
}
