#include "db/ConfigBuilder.hpp"
#include "db/Database.hpp"
#include "fmt/includes.h"
#include "fmt/Preset.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QUrl>

#define BOX_UNDERLYING_DNS dataStore->core_box_underlying_dns.isEmpty() ? "local" : dataStore->core_box_underlying_dns

namespace NekoGui {

    QStringList getAutoBypassExternalProcessPaths(const std::shared_ptr<BuildConfigResult> &result) {
        QStringList paths;
        for (const auto &extR: result->extRs) {
            auto path = extR->program;
            if (path.trimmed().isEmpty()) continue;
            paths << path.replace("\\", "/");
        }
        return paths;
    }

    QString genTunName() {
        auto tun_name = "neko-tun";
#ifdef Q_OS_MACOS
        tun_name = "utun9";
#endif
        return tun_name;
    }

    void MergeJson(QJsonObject &dst, const QJsonObject &src) {
        // 合并
        if (src.isEmpty()) return;
        for (const auto &key: src.keys()) {
            auto v_src = src[key];
            if (dst.contains(key)) {
                auto v_dst = dst[key];
                if (v_src.isObject() && v_dst.isObject()) { // isObject 则合并？
                    auto v_src_obj = v_src.toObject();
                    auto v_dst_obj = v_dst.toObject();
                    MergeJson(v_dst_obj, v_src_obj);
                    dst[key] = v_dst_obj;
                } else {
                    dst[key] = v_src;
                }
            } else if (v_src.isArray()) {
                if (key.startsWith("+")) {
                    auto key2 = SubStrAfter(key, "+");
                    auto v_dst = dst[key2];
                    auto v_src_arr = v_src.toArray();
                    auto v_dst_arr = v_dst.toArray();
                    QJSONARRAY_ADD(v_src_arr, v_dst_arr)
                    dst[key2] = v_src_arr;
                } else if (key.endsWith("+")) {
                    auto key2 = SubStrBefore(key, "+");
                    auto v_dst = dst[key2];
                    auto v_src_arr = v_src.toArray();
                    auto v_dst_arr = v_dst.toArray();
                    QJSONARRAY_ADD(v_dst_arr, v_src_arr)
                    dst[key2] = v_dst_arr;
                } else {
                    dst[key] = v_src;
                }
            } else {
                dst[key] = v_src;
            }
        }
    }

    QString NormalizeDnsStrategy(const QString &strategy) {
        const auto value = strategy.trimmed();
        if (value.compare("AsIs", Qt::CaseInsensitive) == 0) return {};
        return value;
    }

    QJsonObject BuildDomainResolverObject(const QString &server, const QString &strategy = "ipv4_only") {
        QJsonObject resolver{{"server", server}};
        const auto normalizedStrategy = NormalizeDnsStrategy(strategy);
        if (!normalizedStrategy.isEmpty()) resolver["strategy"] = normalizedStrategy;
        return resolver;
    }

    QString StableRouteFluentTag(const QString &prefix, const QString &key) {
        const auto digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
        return prefix + "-" + digest;
    }

    QStringList ParseResolverDohUpstreams(const QString &raw) {
        auto normalized = raw;
        normalized.replace(",", "\n");
        QStringList out;
        QSet<QString> seen;
        for (const auto &line: SplitLinesSkipSharp(normalized)) {
            const auto value = line.trimmed();
            if (value.isEmpty() || seen.contains(value)) continue;
            seen.insert(value);
            out << value;
        }
        return out;
    }

    bool IsValidDohUpstream(const QString &raw, QString *error = nullptr) {
        const auto url = QUrl(raw.trimmed());
        const auto label = raw.trimmed();
        auto setError = [&](const QString &message) {
            if (error != nullptr) *error = QStringLiteral("%1: %2").arg(label, message);
            return false;
        };
        if (!url.isValid() || url.scheme().toLower() != "https") return setError("DoH URL must use https scheme");
        if (url.host().isEmpty()) return setError("DoH URL must have host");
        if (url.path().isEmpty() || url.path() == "/") return setError("DoH URL must have non-root path");
        if (!url.userName().isEmpty() || !url.password().isEmpty()) return setError("DoH URL must not include credentials");
        if (url.hasQuery()) return setError("DoH URL must not include query");
        if (url.hasFragment()) return setError("DoH URL must not include fragment");
        return true;
    }

    QJsonObject BuildRouteFluentDohServer(const QString &tag, const QString &rawUrl) {
        const auto url = QUrl(rawUrl.trimmed());
        const auto host = url.host();
        QJsonObject server{
            {"tag", tag},
            {"type", "https"},
            {"server", host},
            {"server_port", url.port(443)},
            {"path", url.path()},
        };
        if (!IsIpAddress(host)) {
            server["domain_resolver"] = BuildDomainResolverObject("local-system");
            server["tls"] = QJsonObject{
                {"enabled", true},
                {"server_name", host},
            };
        }
        return server;
    }

    QJsonObject BuildRouteFluentResolverGroup(const QString &tag, const QStringList &primaryTags, bool allowLocalFallback) {
        QJsonObject group{
            {"tag", tag},
            {"type", "routefluent_resolver_group"},
        };
        if (primaryTags.isEmpty()) {
            group["mode"] = "local_only";
            group["fallback"] = "local-system";
            group["fallback_enabled"] = true;
            group["fallback_ttl_cap"] = "60s";
            return group;
        }
        group["primary"] = QList2QJsonArray(primaryTags);
        if (allowLocalFallback) {
            group["fallback"] = "local-system";
            group["fallback_enabled"] = true;
            group["probe_domains"] = QJsonArray{"www.gstatic.com", "cloudflare.com"};
            group["failure_threshold"] = 2;
            group["recovery_success_threshold"] = 2;
            group["probe_interval"] = "30s";
            group["unhealthy_cooldown"] = "20s";
            group["fallback_ttl_cap"] = "60s";
        }
        return group;
    }

    bool DnsServersContainTag(const QJsonArray &servers, const QString &tag) {
        for (const auto &value: servers) {
            if (value.isObject() && value.toObject()["tag"].toString() == tag) return true;
        }
        return false;
    }

    void AppendDnsServerIfMissing(QJsonArray &servers, const QJsonObject &server) {
        const auto tag = server["tag"].toString();
        if (!tag.isEmpty() && DnsServersContainTag(servers, tag)) return;
        servers += server;
    }

    void ApplyRouteFluentResolverBindings(const std::shared_ptr<BuildConfigStatus> &status, QJsonArray &dnsServers) {
        if (status->resolverBindingRequests.isEmpty()) return;

        AppendDnsServerIfMissing(dnsServers, QJsonObject{
                                                 {"tag", "local-system"},
                                                 {"type", "local"},
                                             });

        QMap<QString, QString> dohTagByUrl;
        QSet<QString> resolverGroupKeys;
        bool localOnlyGroupAdded = false;

        for (const auto &request: status->resolverBindingRequests) {
            QString resolverTag;
            if (request.dohUpstreams.isEmpty()) {
                resolverTag = "rf-resolver-no-doh-local";
                if (!localOnlyGroupAdded) {
                    AppendDnsServerIfMissing(dnsServers, BuildRouteFluentResolverGroup(resolverTag, {}, true));
                    localOnlyGroupAdded = true;
                }
            } else {
                QStringList primaryTags;
                for (const auto &upstream: request.dohUpstreams) {
                    QString error;
                    if (!IsValidDohUpstream(upstream, &error)) {
                        status->result->error = QStringLiteral("invalid server resolver DoH upstream for %1: %2")
                                                    .arg(request.outboundTag, error);
                        return;
                    }
                    auto normalizedUrl = QUrl(upstream.trimmed()).toString(QUrl::FullyEncoded);
                    if (!dohTagByUrl.contains(normalizedUrl)) {
                        const auto dohTag = StableRouteFluentTag("rf-doh", normalizedUrl);
                        dohTagByUrl[normalizedUrl] = dohTag;
                        AppendDnsServerIfMissing(dnsServers, BuildRouteFluentDohServer(dohTag, normalizedUrl));
                    }
                    primaryTags << dohTagByUrl[normalizedUrl];
                }
                const auto groupKey = primaryTags.join("|") + "|fallback=" + (request.allowLocalFallback ? "1" : "0");
                resolverTag = StableRouteFluentTag("rf-resolver", groupKey);
                if (!resolverGroupKeys.contains(groupKey)) {
                    AppendDnsServerIfMissing(dnsServers, BuildRouteFluentResolverGroup(resolverTag, primaryTags, request.allowLocalFallback));
                    resolverGroupKeys.insert(groupKey);
                }
            }

            if (request.outboundIndex < 0 || request.outboundIndex >= status->outbounds.size()) continue;
            auto outbound = status->outbounds[request.outboundIndex].toObject();
            outbound["domain_resolver"] = BuildDomainResolverObject(resolverTag);
            status->outbounds.replace(request.outboundIndex, outbound);
        }
    }

    void ApplyDnsServerDialOptions(QJsonObject &server, const QString &detour, const QString &domainResolver) {
        const auto type = server["type"].toString();
        if (type == "local" || type == "fakeip" || type == "hosts") return;
        const auto detourTag = detour.trimmed();
        if (!detourTag.isEmpty() && detourTag != "direct" && detourTag != "bypass") server["detour"] = detourTag;
        if (!domainResolver.trimmed().isEmpty()) server["domain_resolver"] = domainResolver.trimmed();
    }

    void ApplyDnsServerHostPort(QJsonObject &server, const QUrl &url) {
        auto host = url.host();
        if (host.isEmpty()) host = url.authority();
        if (!host.isEmpty()) server["server"] = host;
        if (url.port() > 0) server["server_port"] = url.port();
    }

    QJsonObject BuildDnsServer(const QString &tag, const QString &rawAddress, const QString &detour = {}, const QString &domainResolver = {}) {
        auto address = rawAddress.trimmed();
        if (address.isEmpty()) address = "local";

        QJsonObject server;
        if (!tag.trimmed().isEmpty()) server["tag"] = tag.trimmed();

        const auto lowerAddress = address.toLower();
        if (lowerAddress == "local") {
            server["type"] = "local";
            return server;
        }
        if (lowerAddress == "fakeip") {
            server["type"] = "fakeip";
            server["inet4_range"] = "198.18.0.0/15";
            server["inet6_range"] = "fc00::/18";
            return server;
        }

        const auto url = QUrl(address);
        const auto scheme = url.scheme().toLower();
        if (scheme == "https" || scheme == "h3" || scheme == "tls" || scheme == "quic" || scheme == "tcp" || scheme == "udp") {
            server["type"] = scheme;
            ApplyDnsServerHostPort(server, url);
            if ((scheme == "https" || scheme == "h3") && !url.path().isEmpty() && url.path() != "/dns-query") {
                server["path"] = url.path();
            }
        } else if (scheme == "dhcp") {
            server["type"] = "dhcp";
            const auto interfaceName = url.host();
            if (!interfaceName.isEmpty() && interfaceName != "auto") server["interface"] = interfaceName;
        } else {
            server["type"] = "udp";
            auto host = address;
            if (address.startsWith("[") || address.count(":") == 1) {
                const auto plainUrl = QUrl("udp://" + address);
                if (!plainUrl.host().isEmpty()) {
                    host = plainUrl.host();
                    if (plainUrl.port() > 0) server["server_port"] = plainUrl.port();
                }
            }
            server["server"] = host;
        }

        ApplyDnsServerDialOptions(server, detour, domainResolver);
        return server;
    }

    void SetRouteOutboundOrAction(QJsonObject &rule, const QString &outbound) {
        if (outbound == "block") {
            rule.remove("outbound");
            rule["action"] = "reject";
        } else if (outbound == "dns-out") {
            rule.remove("outbound");
            rule["action"] = "hijack-dns";
        } else {
            rule["outbound"] = outbound;
        }
    }

    void AppendInboundRouteActions(const std::shared_ptr<BuildConfigStatus> &status, const QString &inboundTag) {
        const auto inboundDomainStrategy = NormalizeDnsStrategy(dataStore->routing->domain_strategy);
        if (!inboundDomainStrategy.isEmpty()) {
            status->routingRules += QJsonObject{
                {"inbound", inboundTag},
                {"action", "resolve"},
                {"strategy", inboundDomainStrategy},
            };
        }
        if (dataStore->routing->sniffing_mode != SniffingMode::DISABLE) {
            status->routingRules += QJsonObject{
                {"inbound", inboundTag},
                {"action", "sniff"},
            };
        }
    }

    QJsonObject NormalizeRouteRuleActions(QJsonObject rule);

    QJsonArray NormalizeRouteRuleActions(const QJsonArray &rules) {
        QJsonArray normalized;
        for (const auto &value: rules) {
            if (value.isObject()) {
                normalized += NormalizeRouteRuleActions(value.toObject());
            } else {
                normalized += value;
            }
        }
        return normalized;
    }

    QJsonObject NormalizeRouteRuleActions(QJsonObject rule) {
        if (rule["rules"].isArray()) {
            rule["rules"] = NormalizeRouteRuleActions(rule["rules"].toArray());
        }
        const auto outbound = rule["outbound"].toString();
        if (outbound == "block" || outbound == "dns-out") {
            SetRouteOutboundOrAction(rule, outbound);
        }
        return rule;
    }

    // Common

    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport) {
        auto result = std::make_shared<BuildConfigResult>();
        auto status = std::make_shared<BuildConfigStatus>();
        status->ent = ent;
        status->result = result;
        status->forTest = forTest;
        status->forExport = forExport;

        auto customBean = dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get());
        if (customBean != nullptr && customBean->core == "internal-full") {
            result->coreConfig = QString2QJsonObject(customBean->config_simple);
        } else {
            BuildConfigSingBox(status);
        }

        // apply custom config
        MergeJson(result->coreConfig, QString2QJsonObject(ent->bean->custom_config));

        return result;
    }

    QString BuildChain(int chainId, const std::shared_ptr<BuildConfigStatus> &status) {
        auto group = profileManager->GetGroup(status->ent->gid);
        if (group == nullptr) {
            status->result->error = QStringLiteral("This profile is not in any group, your data may be corrupted.");
            return {};
        }

        auto resolveChain = [=](const std::shared_ptr<ProxyEntity> &ent) {
            QList<std::shared_ptr<ProxyEntity>> resolved;
            if (ent->type == "chain") {
                auto list = ent->ChainBean()->list;
                std::reverse(std::begin(list), std::end(list));
                for (auto id: list) {
                    resolved += profileManager->GetProfile(id);
                    if (resolved.last() == nullptr) {
                        status->result->error = QStringLiteral("chain missing ent: %1").arg(id);
                        break;
                    }
                    if (resolved.last()->type == "chain") {
                        status->result->error = QStringLiteral("chain in chain is not allowed: %1").arg(id);
                        break;
                    }
                }
            } else {
                resolved += ent;
            };
            return resolved;
        };

        // Make list
        auto ents = resolveChain(status->ent);
        if (!status->result->error.isEmpty()) return {};

        if (group->front_proxy_id >= 0) {
            auto fEnt = profileManager->GetProfile(group->front_proxy_id);
            if (fEnt == nullptr) {
                status->result->error = QStringLiteral("front proxy ent not found.");
                return {};
            }
            ents += resolveChain(fEnt);
            if (!status->result->error.isEmpty()) return {};
        }

        // BuildChain
        QString chainTagOut = BuildChainInternal(0, ents, status);

        // Chain ent traffic stat
        if (ents.length() > 1) {
            status->ent->traffic_data->id = status->ent->id;
            status->ent->traffic_data->tag = chainTagOut.toStdString();
            status->result->outboundStats += status->ent->traffic_data;
        }

        return chainTagOut;
    }

    QString EffectiveResolverDohUpstreams(const std::shared_ptr<ProxyEntity> &ent) {
        if (ent == nullptr || ent->bean == nullptr) return {};
        const auto profileResolver = ent->bean->serverResolverDohUpstreams.trimmed();
        if (!profileResolver.isEmpty() || !ent->bean->inheritSubscriptionResolver) return profileResolver;

        auto group = profileManager->GetGroup(ent->gid);
        if (group == nullptr) return {};
        return group->default_server_resolver_doh.trimmed();
    }

    bool EffectiveResolverAllowLocalFallback(const std::shared_ptr<ProxyEntity> &ent) {
        if (ent == nullptr || ent->bean == nullptr) return true;
        if (!ent->bean->serverResolverDohUpstreams.trimmed().isEmpty() || !ent->bean->inheritSubscriptionResolver) {
            return ent->bean->serverResolverAllowLocalFallback;
        }
        auto group = profileManager->GetGroup(ent->gid);
        if (group == nullptr || group->default_server_resolver_doh.trimmed().isEmpty()) return ent->bean->serverResolverAllowLocalFallback;
        return group->default_server_resolver_allow_local_fallback;
    }

    void ApplySubscriptionAnyTLSClientDefault(const std::shared_ptr<ProxyEntity> &ent, QJsonObject &outbound) {
        if (ent == nullptr || ent->bean == nullptr || ent->type != "anytls" || !ent->bean->inheritSubscriptionClient) return;
        if (outbound.contains("client")) return;

        auto group = profileManager->GetGroup(ent->gid);
        if (group == nullptr) return;

        const auto mode = group->default_client_mode.trimmed().toLower();
        if (mode == "mihomo") {
            outbound["client"] = group->default_client_value.trimmed().isEmpty() ? "mihomo/1.19.28" : group->default_client_value.trimmed();
        } else if (mode == "custom" && !group->default_client_value.trimmed().isEmpty()) {
            outbound["client"] = group->default_client_value.trimmed();
        }
    }

#define DOMAIN_USER_RULE                                                             \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->proxy_domain)) {  \
        if (dataStore->routing->dns_routing) status->domainListDNSRemote += line;    \
        status->domainListRemote += line;                                            \
    }                                                                                \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->direct_domain)) { \
        if (dataStore->routing->dns_routing) status->domainListDNSDirect += line;    \
        status->domainListDirect += line;                                            \
    }                                                                                \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->block_domain)) {  \
        status->domainListBlock += line;                                             \
    }

#define IP_USER_RULE                                                             \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->block_ip)) {  \
        status->ipListBlock += line;                                             \
    }                                                                            \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->proxy_ip)) {  \
        status->ipListRemote += line;                                            \
    }                                                                            \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->direct_ip)) { \
        status->ipListDirect += line;                                            \
    }

    QString BuildChainInternal(int chainId, const QList<std::shared_ptr<ProxyEntity>> &ents,
                               const std::shared_ptr<BuildConfigStatus> &status) {
        QString chainTag = "c-" + Int2String(chainId);
        QString chainTagOut;
        bool muxApplied = false;

        QString pastTag;
        int pastExternalStat = 0;
        int index = 0;

        for (const auto &ent: ents) {
            // tagOut: sing-box outbound tag for a profile
            // profile2 (in) (global)   tag g-(id)
            // profile1                 tag (chainTag)-(id)
            // profile0 (out)           tag (chainTag)-(id) / single: chainTag=g-(id)
            auto tagOut = chainTag + "-" + Int2String(ent->id);

            // needGlobal: can only contain one?
            bool needGlobal = false;

            // first profile set as global
            auto isFirstProfile = index == ents.length() - 1;
            if (isFirstProfile) {
                needGlobal = true;
                tagOut = "g-" + Int2String(ent->id);
            }

            // last profile set as "proxy"
            if (chainId == 0 && index == 0) {
                needGlobal = false;
                tagOut = "proxy";
            }

            // ignoreConnTag
            if (index != 0) {
                status->result->ignoreConnTag << tagOut;
            }

            const auto globalAlreadyBuilt = needGlobal && status->globalProfiles.contains(ent->id);

            if (index > 0) {
                // chain rules: past
                if (pastExternalStat == 0) {
                    auto replaced = status->outbounds.last().toObject();
                    replaced["detour"] = tagOut;
                    status->outbounds.removeLast();
                    status->outbounds += replaced;
                } else {
                    status->routingRules += QJsonObject{
                        {"inbound", QJsonArray{pastTag + "-mapping"}},
                        {"outbound", tagOut},
                    };
                }
            } else {
                // index == 0 means last profile in chain / not chain
                chainTagOut = tagOut;
                if (chainId == 0) status->result->outboundStat = ent->traffic_data;
            }

            if (globalAlreadyBuilt) {
                pastTag = tagOut;
                pastExternalStat = 0;
                index++;
                continue;
            }

            if (needGlobal) {
                status->globalProfiles += ent->id;
            }

            // chain rules: this
            auto ext_mapping_port = 0;
            auto ext_socks_port = 0;
            auto thisExternalStat = ent->bean->NeedExternal(isFirstProfile);
            if (thisExternalStat < 0) {
                status->result->error = "This configuration cannot be set automatically, please try another.";
                return {};
            }

            // determine port
            if (thisExternalStat > 0) {
                if (ent->type == "custom") {
                    auto bean = ent->CustomBean();
                    if (IsValidPort(bean->mapping_port)) {
                        ext_mapping_port = bean->mapping_port;
                    } else {
                        ext_mapping_port = MkPort();
                    }
                    if (IsValidPort(bean->socks_port)) {
                        ext_socks_port = bean->socks_port;
                    } else {
                        ext_socks_port = MkPort();
                    }
                } else {
                    ext_mapping_port = MkPort();
                    ext_socks_port = MkPort();
                }
            }
            if (thisExternalStat == 2) dataStore->need_keep_vpn_off = true;
            if (thisExternalStat == 1) {
                // mapping
                status->inbounds += QJsonObject{
                    {"type", "direct"},
                    {"tag", tagOut + "-mapping"},
                    {"listen", "127.0.0.1"},
                    {"listen_port", ext_mapping_port},
                    {"override_address", ent->bean->serverAddress},
                    {"override_port", ent->bean->serverPort},
                };
                // no chain rule and not outbound, so need to set to direct
                if (isFirstProfile) {
                    status->routingRules += QJsonObject{
                        {"inbound", QJsonArray{tagOut + "-mapping"}},
                        {"outbound", "direct"},
                    };
                }
            }

            // Outbound

            QJsonObject outbound;
            auto stream = GetStreamSettings(ent->bean.get());

            if (thisExternalStat > 0) {
                auto extR = ent->bean->BuildExternal(ext_mapping_port, ext_socks_port, thisExternalStat);
                if (extR.program.isEmpty()) {
                    status->result->error = QObject::tr("Core not found: %1").arg(ent->bean->DisplayCoreType());
                    return {};
                }
                if (!extR.error.isEmpty()) { // rejected
                    status->result->error = extR.error;
                    return {};
                }
                extR.tag = ent->bean->DisplayType();
                status->result->extRs.emplace_back(std::make_shared<NekoGui_fmt::ExternalBuildResult>(extR));

                // SOCKS OUTBOUND
                outbound["type"] = "socks";
                outbound["server"] = "127.0.0.1";
                outbound["server_port"] = ext_socks_port;
            } else {
                const auto coreR = ent->bean->BuildCoreObjSingBox();
                if (coreR.outbound.isEmpty()) {
                    status->result->error = "unsupported outbound";
                    return {};
                }
                if (!coreR.error.isEmpty()) { // rejected
                    status->result->error = coreR.error;
                    return {};
                }
                outbound = coreR.outbound;
            }

            ApplySubscriptionAnyTLSClientDefault(ent, outbound);

            // outbound misc
            outbound["tag"] = tagOut;
            ent->traffic_data->id = ent->id;
            ent->traffic_data->tag = tagOut.toStdString();
            status->result->outboundStats += ent->traffic_data;

            // mux common
            auto needMux = ent->type == "vmess" || ent->type == "trojan" || ent->type == "vless";
            needMux &= dataStore->mux_concurrency > 0;

            if (stream != nullptr) {
                if (stream->network == "grpc" || stream->network == "quic" || (stream->network == "http" && stream->security == "tls")) {
                    needMux = false;
                }
                if (stream->multiplex_status == 0) {
                    if (!dataStore->mux_default_on) needMux = false;
                } else if (stream->multiplex_status == 1) {
                    needMux = true;
                } else if (stream->multiplex_status == 2) {
                    needMux = false;
                }
            }
            if (ent->type == "vless" && outbound["flow"] != "") {
                needMux = false;
            }

            // common
            // apply domain_strategy
            outbound["domain_strategy"] = dataStore->routing->outbound_domain_strategy;
            // apply mux
            if (!muxApplied && needMux) {
                auto muxObj = QJsonObject{
                    {"enabled", true},
                    {"protocol", dataStore->mux_protocol},
                    {"padding", dataStore->mux_padding},
                    {"max_streams", dataStore->mux_concurrency},
                };
                outbound["multiplex"] = muxObj;
                muxApplied = true;
            }

            // apply custom outbound settings
            MergeJson(outbound, QString2QJsonObject(ent->bean->custom_outbound));

            const auto outboundServer = outbound["server"].toString().trimmed();
            if (!outboundServer.isEmpty() && !IsIpAddress(outboundServer)) {
                status->resolverBindingRequests += ResolverBindingRequest{
                    static_cast<int>(status->outbounds.size()),
                    tagOut,
                    outboundServer,
                    ParseResolverDohUpstreams(EffectiveResolverDohUpstreams(ent)),
                    EffectiveResolverAllowLocalFallback(ent),
                };
            }

            status->outbounds += outbound;
            pastTag = tagOut;
            pastExternalStat = thisExternalStat;
            index++;
        }

        return chainTagOut;
    }

    // SingBox

    void BuildConfigSingBox(const std::shared_ptr<BuildConfigStatus> &status) {
        // Log
        status->result->coreConfig["log"] = QJsonObject{{"level", dataStore->log_level}};

        // Inbounds

        // mixed-in
        if (IsValidPort(dataStore->inbound_socks_port) && !status->forTest) {
            QJsonObject inboundObj;
            inboundObj["tag"] = "mixed-in";
            inboundObj["type"] = "mixed";
            inboundObj["listen"] = dataStore->inbound_address;
            inboundObj["listen_port"] = dataStore->inbound_socks_port;
            if (dataStore->inbound_auth->NeedAuth()) {
                inboundObj["users"] = QJsonArray{
                    QJsonObject{
                        {"username", dataStore->inbound_auth->username},
                        {"password", dataStore->inbound_auth->password},
                    },
                };
            }
            status->inbounds += inboundObj;
            AppendInboundRouteActions(status, "mixed-in");
        }

        // tun-in
        if (dataStore->vpn_internal_tun && dataStore->spmode_vpn && !status->forTest) {
            QJsonObject inboundObj;
            inboundObj["tag"] = "tun-in";
            inboundObj["type"] = "tun";
            inboundObj["interface_name"] = genTunName();
            inboundObj["auto_route"] = true;
            inboundObj["endpoint_independent_nat"] = true;
            inboundObj["mtu"] = dataStore->vpn_mtu;
            inboundObj["stack"] = Preset::SingBox::VpnImplementation.value(dataStore->vpn_implementation);
            inboundObj["strict_route"] = dataStore->vpn_strict_route;
            QJsonArray tunAddress{"172.19.0.1/28"};
            if (dataStore->vpn_ipv6) tunAddress += "fdfe:dcba:9876::1/126";
            inboundObj["address"] = tunAddress;
            status->inbounds += inboundObj;
            AppendInboundRouteActions(status, "tun-in");
        }

        // Outbounds
        auto tagProxy = BuildChain(0, status);
        if (!status->result->error.isEmpty()) return;

        if (!status->forTest && !status->forExport) {
            int auxChainId = 1000;
            const auto mainEnt = status->ent;
            for (auto it = dataStore->aux_profile_ports.constBegin(); it != dataStore->aux_profile_ports.constEnd(); ++it) {
                auto auxProfile = profileManager->GetProfile(it.key());
                if (auxProfile == nullptr || auxProfile->bean == nullptr) continue;
                auto auxGroup = profileManager->GetGroup(auxProfile->gid);
                if (auxGroup == nullptr || auxGroup->archive) continue;
                if (!IsValidPort(it.value())) continue;

                const auto inboundTag = QStringLiteral("aux-mixed-%1").arg(auxProfile->id);
                status->inbounds += QJsonObject{
                    {"tag", inboundTag},
                    {"type", "mixed"},
                    {"listen", "127.0.0.1"},
                    {"listen_port", it.value()},
                };
                AppendInboundRouteActions(status, inboundTag);

                status->ent = auxProfile;
                const auto auxOutboundTag = BuildChain(auxChainId++, status);
                status->ent = mainEnt;
                if (!status->result->error.isEmpty()) return;
                if (auxOutboundTag.isEmpty()) continue;

                status->routingRules += QJsonObject{
                    {"inbound", QJsonArray{inboundTag}},
                    {"outbound", auxOutboundTag},
                };
            }
            status->ent = mainEnt;
        }

        // direct & bypass
        status->outbounds += QJsonObject{
            {"type", "direct"},
            {"tag", "direct"},
        };
        status->outbounds += QJsonObject{
            {"type", "direct"},
            {"tag", "bypass"},
        };

        // custom inbound
        if (!status->forTest) QJSONARRAY_ADD(status->inbounds, QString2QJsonObject(dataStore->custom_inbound)["inbounds"].toArray())

        status->result->coreConfig.insert("inbounds", status->inbounds);

        // user rule
        if (!status->forTest) {
            DOMAIN_USER_RULE
            IP_USER_RULE
        }

        // sing-box common rule object
        auto make_rule = [&](const QStringList &list, bool isIP = false) {
            QJsonObject rule;
            //
            QJsonArray ip_cidr;
            QJsonArray geoip;
            //
            QJsonArray domain_keyword;
            QJsonArray domain_subdomain;
            QJsonArray domain_regexp;
            QJsonArray domain_full;
            QJsonArray geosite;
            for (auto item: list) {
                if (isIP) {
                    if (item.startsWith("geoip:")) {
                        geoip += item.replace("geoip:", "");
                    } else {
                        ip_cidr += item;
                    }
                } else {
                    // Legacy routing syntax mapped to sing-box route fields.
                    if (item.startsWith("geosite:")) {
                        geosite += item.replace("geosite:", "");
                    } else if (item.startsWith("full:")) {
                        domain_full += item.replace("full:", "").toLower();
                    } else if (item.startsWith("domain:")) {
                        domain_subdomain += item.replace("domain:", "").toLower();
                    } else if (item.startsWith("regexp:")) {
                        domain_regexp += item.replace("regexp:", "").toLower();
                    } else if (item.startsWith("keyword:")) {
                        domain_keyword += item.replace("keyword:", "").toLower();
                    } else {
                        domain_subdomain += item.toLower();
                    }
                }
            }
            if (isIP) {
                if (ip_cidr.isEmpty() && geoip.isEmpty()) return rule;
                rule["ip_cidr"] = ip_cidr;
                rule["geoip"] = geoip;
            } else {
                if (domain_keyword.isEmpty() && domain_subdomain.isEmpty() && domain_regexp.isEmpty() && domain_full.isEmpty() && geosite.isEmpty()) {
                    return rule;
                }
                rule["domain"] = domain_full;
                rule["domain_suffix"] = domain_subdomain;
                rule["domain_keyword"] = domain_keyword;
                rule["domain_regex"] = domain_regexp;
                rule["geosite"] = geosite;
            }
            return rule;
        };

        // final add DNS
        QJsonObject dns;
        QJsonArray dnsServers;
        QJsonArray dnsRules;

        // Remote
        if (!status->forTest)
            dnsServers += BuildDnsServer("dns-remote", dataStore->routing->remote_dns, tagProxy, "dns-local");

        // Direct
        auto directObj = BuildDnsServer("dns-direct", dataStore->routing->direct_dns, "direct", "dns-local");
        if (dataStore->routing->dns_final_out == "bypass") {
            dnsServers.prepend(directObj);
        } else {
            dnsServers.append(directObj);
        }

        // Fakedns
        if (dataStore->fake_dns && dataStore->vpn_internal_tun && dataStore->spmode_vpn && !status->forTest) {
            dnsServers += BuildDnsServer("dns-fake", "fakeip");
        }

        // Underlying 100% Working DNS ?
        dnsServers += BuildDnsServer("dns-local", BOX_UNDERLYING_DNS, "direct");

        // sing-box dns rule object
        auto add_rule_dns = [&](const QStringList &list, const QString &server, const QString &strategy = {}) {
            auto rule = make_rule(list, false);
            if (rule.isEmpty()) return;
            rule["action"] = "route";
            rule["server"] = server;
            const auto normalizedStrategy = NormalizeDnsStrategy(strategy);
            if (!normalizedStrategy.isEmpty()) rule["strategy"] = normalizedStrategy;
            dnsRules += rule;
        };
        add_rule_dns(status->domainListDNSRemote, "dns-remote", dataStore->routing->remote_dns_strategy);
        add_rule_dns(status->domainListDNSDirect, "dns-direct", dataStore->routing->direct_dns_strategy);

        // built-in rules
        if (!status->forTest) {
            dnsRules += QJsonObject{
                {"query_type", QJsonArray{32, 33}},
                {"action", "predefined"},
                {"rcode", "NOERROR"},
            };
            dnsRules += QJsonObject{
                {"domain_suffix", ".lan"},
                {"action", "predefined"},
                {"rcode", "NOERROR"},
            };
        }

        // fakedns rule
        if (dataStore->fake_dns && dataStore->vpn_internal_tun && dataStore->spmode_vpn && !status->forTest) {
            dnsRules += QJsonObject{
                {"inbound", "tun-in"},
                {"action", "route"},
                {"server", "dns-fake"},
            };
        }

        dns["servers"] = dnsServers;
        dns["rules"] = dnsRules;
        dns["independent_cache"] = true;
        const auto defaultDnsStrategy = NormalizeDnsStrategy(
            dataStore->routing->dns_final_out == "bypass" ? dataStore->routing->direct_dns_strategy : dataStore->routing->remote_dns_strategy);
        if (!defaultDnsStrategy.isEmpty()) dns["strategy"] = defaultDnsStrategy;

        if (dataStore->routing->use_dns_object) {
            dns = QString2QJsonObject(dataStore->routing->dns_object);
            dnsServers = dns["servers"].toArray();
        }
        ApplyRouteFluentResolverBindings(status, dnsServers);
        if (!status->result->error.isEmpty()) return;
        dns["servers"] = dnsServers;
        status->result->coreConfig.insert("dns", dns);
        status->result->coreConfig.insert("outbounds", status->outbounds);

        // Routing

        // dns hijack
        if (!status->forTest) {
            status->routingRules += QJsonObject{
                {"protocol", "dns"},
                {"action", "hijack-dns"},
            };
        }

        // sing-box routing rule object
        auto add_rule_route = [&](const QStringList &list, bool isIP, const QString &out) {
            auto rule = make_rule(list, isIP);
            if (rule.isEmpty()) return;
            SetRouteOutboundOrAction(rule, out);
            status->routingRules += rule;
        };

        // final add user rule
        add_rule_route(status->domainListBlock, false, "block");
        add_rule_route(status->domainListRemote, false, tagProxy);
        add_rule_route(status->domainListDirect, false, "bypass");
        add_rule_route(status->ipListBlock, true, "block");
        add_rule_route(status->ipListRemote, true, tagProxy);
        add_rule_route(status->ipListDirect, true, "bypass");

        // built-in rules
        status->routingRules += QJsonObject{
            {"network", "udp"},
            {"port", QJsonArray{135, 137, 138, 139, 5353}},
            {"action", "reject"},
        };
        status->routingRules += QJsonObject{
            {"ip_cidr", QJsonArray{"224.0.0.0/3", "ff00::/8"}},
            {"action", "reject"},
        };
        status->routingRules += QJsonObject{
            {"source_ip_cidr", QJsonArray{"224.0.0.0/3", "ff00::/8"}},
            {"action", "reject"},
        };

        // tun user rule
        if (dataStore->vpn_internal_tun && dataStore->spmode_vpn && !status->forTest) {
            auto match_out = dataStore->vpn_rule_white ? "proxy" : "bypass";

            QString process_name_rule = dataStore->vpn_rule_process.trimmed();
            if (!process_name_rule.isEmpty()) {
                auto arr = SplitLinesSkipSharp(process_name_rule);
                QJsonObject rule{{"outbound", match_out},
                                 {"process_name", QList2QJsonArray(arr)}};
                status->routingRules += rule;
            }

            QString cidr_rule = dataStore->vpn_rule_cidr.trimmed();
            if (!cidr_rule.isEmpty()) {
                auto arr = SplitLinesSkipSharp(cidr_rule);
                QJsonObject rule{{"outbound", match_out},
                                 {"ip_cidr", QList2QJsonArray(arr)}};
                status->routingRules += rule;
            }

            auto autoBypassExternalProcessPaths = getAutoBypassExternalProcessPaths(status->result);
            if (!autoBypassExternalProcessPaths.isEmpty()) {
                QJsonObject rule{{"outbound", "bypass"},
                                 {"process_name", QList2QJsonArray(autoBypassExternalProcessPaths)}};
                status->routingRules += rule;
            }
        }

        // geopath
        auto geoip = FindCoreAsset("geoip.db");
        auto geosite = FindCoreAsset("geosite.db");
        if (geoip.isEmpty()) status->result->error = +"geoip.db not found";
        if (geosite.isEmpty()) status->result->error = +"geosite.db not found";

        // final add routing rule
        auto routingRules = NormalizeRouteRuleActions(QString2QJsonObject(dataStore->routing->custom)["rules"].toArray());
        if (status->forTest) routingRules = {};
        auto globalRoutingRules = NormalizeRouteRuleActions(QString2QJsonObject(dataStore->custom_route_global)["rules"].toArray());
        if (!status->forTest) QJSONARRAY_ADD(routingRules, globalRoutingRules)
        QJSONARRAY_ADD(routingRules, status->routingRules)
        auto finalOutbound = dataStore->routing->def_outbound;
        if (!status->forTest && finalOutbound == "block") {
            routingRules += QJsonObject{{"action", "reject"}};
            finalOutbound = "direct";
        }
        auto routeObj = QJsonObject{
            {"rules", routingRules},
            {"auto_detect_interface", dataStore->spmode_vpn}, // TODO force enable?
            {
                "geoip",
                QJsonObject{
                    {"path", geoip},
                },
            },
            {
                "geosite",
                QJsonObject{
                    {"path", geosite},
                },
            }};
        if (!status->forTest) {
            routeObj["final"] = finalOutbound;
            if (!dataStore->routing->use_dns_object) {
                routeObj["default_domain_resolver"] = QJsonObject{{"server", "dns-direct"}};
            }
        }
        if (status->forExport) {
            routeObj.remove("geoip");
            routeObj.remove("geosite");
            routeObj.remove("auto_detect_interface");
        }
        status->result->coreConfig.insert("route", routeObj);

        // experimental
        QJsonObject experimentalObj;

        if (!status->forTest && dataStore->core_box_clash_api > 0) {
            QJsonObject clash_api = {
                {"external_controller", "127.0.0.1:" + Int2String(dataStore->core_box_clash_api)},
                {"secret", dataStore->core_box_clash_api_secret},
                {"external_ui", "dashboard"},
            };
            experimentalObj["clash_api"] = clash_api;
        }

        if (!experimentalObj.isEmpty()) status->result->coreConfig.insert("experimental", experimentalObj);
    }

    QString WriteVPNSingBoxConfig() {
        // tun user rule
        auto match_out = dataStore->vpn_rule_white ? "neko-socks" : "direct";
        auto no_match_out = dataStore->vpn_rule_white ? "direct" : "neko-socks";

        QString process_name_rule = dataStore->vpn_rule_process.trimmed();
        if (!process_name_rule.isEmpty()) {
            auto arr = SplitLinesSkipSharp(process_name_rule);
            QJsonObject rule{{"outbound", match_out},
                             {"process_name", QList2QJsonArray(arr)}};
            process_name_rule = "," + QJsonObject2QString(rule, false);
        }

        QString cidr_rule = dataStore->vpn_rule_cidr.trimmed();
        if (!cidr_rule.isEmpty()) {
            auto arr = SplitLinesSkipSharp(cidr_rule);
            QJsonObject rule{{"outbound", match_out},
                             {"ip_cidr", QList2QJsonArray(arr)}};
            cidr_rule = "," + QJsonObject2QString(rule, false);
        }

        // TODO bypass ext core process path?

        // auth
        QString socks_user_pass;
        if (dataStore->inbound_auth->NeedAuth()) {
            socks_user_pass = R"( "username": "%1", "password": "%2", )";
            socks_user_pass = socks_user_pass.arg(dataStore->inbound_auth->username, dataStore->inbound_auth->password);
        }
        // gen config
        auto configFn = ":/neko/vpn/sing-box-vpn.json";
        if (QFile::exists("vpn/sing-box-vpn.json")) configFn = "vpn/sing-box-vpn.json";
        auto config = ReadFileText(configFn)
                          .replace("//%IPV6_ADDRESS%", dataStore->vpn_ipv6 ? R"(,"fdfe:dcba:9876::1/126")" : "")
                          .replace("//%SOCKS_USER_PASS%", socks_user_pass)
                          .replace("//%PROCESS_NAME_RULE%", process_name_rule)
                          .replace("//%CIDR_RULE%", cidr_rule)
                          .replace("%MTU%", Int2String(dataStore->vpn_mtu))
                          .replace("%STACK%", Preset::SingBox::VpnImplementation.value(dataStore->vpn_implementation))
                          .replace("%TUN_NAME%", genTunName())
                          .replace("%STRICT_ROUTE%", dataStore->vpn_strict_route ? "true" : "false")
                          .replace("%FINAL_OUT%", no_match_out)
                          .replace("%DNS_DIRECT_SERVER%", QJsonObject2QString(BuildDnsServer("dns-direct", BOX_UNDERLYING_DNS, "direct"), false))
                          .replace("%FAKE_DNS_INBOUND%", dataStore->fake_dns ? "tun-in" : "empty")
                          .replace("%PORT%", Int2String(dataStore->inbound_socks_port));
        // write config
        QFile file;
        file.setFileName(QFileInfo(configFn).fileName());
        file.open(QIODevice::ReadWrite | QIODevice::Truncate);
        file.write(config.toUtf8());
        file.close();
        return QFileInfo(file).absoluteFilePath();
    }

    QString WriteVPNLinuxScript(const QString &configPath) {
#ifdef Q_OS_WIN
        return {};
#endif
        // gen script
        auto scriptFn = ":/neko/vpn/vpn-run-root.sh";
        if (QFile::exists("vpn/vpn-run-root.sh")) scriptFn = "vpn/vpn-run-root.sh";
        auto script = ReadFileText(scriptFn)
                          .replace("./nekobox_core", QApplication::applicationDirPath() + "/nekobox_core")
                          .replace("$CONFIG_PATH", configPath);
        // write script
        QFile file2;
        file2.setFileName(QFileInfo(scriptFn).fileName());
        file2.open(QIODevice::ReadWrite | QIODevice::Truncate);
        file2.write(script.toUtf8());
        file2.close();
        return QFileInfo(file2).absoluteFilePath();
    }

} // namespace NekoGui
