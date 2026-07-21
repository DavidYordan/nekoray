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
        return server;
    }

    QJsonObject BuildRouteFluentResolverGroup(const QString &tag, const QStringList &primaryTags) {
        return QJsonObject{
            {"tag", tag},
            {"type", "routefluent_resolver_group"},
            {"primary", QList2QJsonArray(primaryTags)},
        };
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

        QMap<QString, QString> dohTagByUrl;
        QSet<QString> resolverGroupKeys;

        for (const auto &request: status->resolverBindingRequests) {
            QString resolverTag;
            if (!request.dohUpstreams.isEmpty()) {
                QStringList primaryTags;
                for (const auto &upstream: request.dohUpstreams) {
                    QString error;
                    if (!IsValidDohUpstream(upstream, &error)) {
                        status->result->error = QStringLiteral("invalid server resolver DoH upstream for %1: %2")
                                                    .arg(request.outboundTag, error);
                        return;
                    }
                    auto normalizedUrl = QUrl(upstream.trimmed()).toString(QUrl::FullyEncoded);
                    const auto dohHost = QUrl(normalizedUrl).host();
                    if (!IsIpAddress(dohHost)) {
                        status->result->error = QStringLiteral(
                            "server resolver DoH endpoint '%1' requires an explicit, auditable bootstrap IP; "
                            "local-system bootstrap and direct DNS fallback are disabled.")
                                                    .arg(dohHost);
                        return;
                    }
                    if (!dohTagByUrl.contains(normalizedUrl)) {
                        const auto dohTag = StableRouteFluentTag("rf-doh", normalizedUrl);
                        dohTagByUrl[normalizedUrl] = dohTag;
                        AppendDnsServerIfMissing(dnsServers, BuildRouteFluentDohServer(dohTag, normalizedUrl));
                    }
                    primaryTags << dohTagByUrl[normalizedUrl];
                }
                const auto groupKey = primaryTags.join("|");
                resolverTag = StableRouteFluentTag("rf-resolver", groupKey);
                if (!resolverGroupKeys.contains(groupKey)) {
                    AppendDnsServerIfMissing(dnsServers, BuildRouteFluentResolverGroup(resolverTag, primaryTags));
                    resolverGroupKeys.insert(groupKey);
                }
            } else {
                // No subscription/provider DoH means no RouteFluent extension.
                // Preserve NekoRay/sing-box's normal domain resolution path
                // instead of forcing every domain outbound through a custom
                // local-only resolver group.
                continue;
            }

            if (request.outboundIndex < 0 || request.outboundIndex >= status->outbounds.size()) continue;
            auto outbound = status->outbounds[request.outboundIndex].toObject();
            outbound["domain_resolver"] = BuildDomainResolverObject(resolverTag);
            status->outbounds.replace(request.outboundIndex, outbound);
        }
    }

    void ApplyDnsServerDialOptions(QJsonObject &server, const QString &detour, const QString &domainResolver) {
        const auto type = server.value("type").toString();
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

    void AppendInboundRouteActions(QJsonArray &rules, const QString &inboundTag, bool includeResolve = true) {
        const auto inboundDomainStrategy = NormalizeDnsStrategy(dataStore->routing->domain_strategy);
        if (includeResolve && !inboundDomainStrategy.isEmpty()) {
            rules += QJsonObject{
                {"inbound", inboundTag},
                {"action", "resolve"},
                {"strategy", inboundDomainStrategy},
            };
        }
        if (dataStore->routing->sniffing_mode != SniffingMode::DISABLE) {
            rules += QJsonObject{
                {"inbound", inboundTag},
                {"action", "sniff"},
            };
        }
    }

    void AppendInboundRouteActions(const std::shared_ptr<BuildConfigStatus> &status, const QString &inboundTag) {
        AppendInboundRouteActions(status->routingRules, inboundTag);
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
        const auto nestedRules = rule.value("rules");
        if (nestedRules.isArray()) {
            rule["rules"] = NormalizeRouteRuleActions(nestedRules.toArray());
        }
        const auto outbound = rule.value("outbound").toString();
        if (outbound == "block" || outbound == "dns-out") {
            SetRouteOutboundOrAction(rule, outbound);
        }
        return rule;
    }

    bool JsonInboundConstraintMayMatch(const QJsonValue &value, const QString &inboundTag) {
        if (value.isString()) return value.toString() == inboundTag;
        if (!value.isArray()) return true;

        const auto values = value.toArray();
        // sing-box treats an empty inbound list as no constraint, i.e. the
        // rule can match every inbound.  It is not an empty match set.
        if (values.isEmpty()) return true;
        for (const auto &entry: values) {
            // Invalid or non-string constraints are left to sing-box to reject,
            // but must not make this safety check assume the rule is harmless.
            if (!entry.isString() || entry.toString() == inboundTag) return true;
        }
        return false;
    }

    bool RouteRuleMayMatchInbound(const QJsonObject &rule, const QString &inboundTag) {
        // Inverted and malformed rules are deliberately treated conservatively.
        if (rule["invert"].toBool(false)) return true;
        if (rule.contains("inbound") && !JsonInboundConstraintMayMatch(rule["inbound"], inboundTag)) return false;

        if (!rule.contains("rules")) return true;
        if (!rule["rules"].isArray()) return true;

        const auto nestedRules = rule["rules"].toArray();
        const auto mode = rule["mode"].toString().trimmed().toLower();
        if (mode == "and") {
            for (const auto &value: nestedRules) {
                if (value.isObject() && !RouteRuleMayMatchInbound(value.toObject(), inboundTag)) return false;
            }
            return true;
        }
        if (mode == "or") {
            for (const auto &value: nestedRules) {
                if (!value.isObject() || RouteRuleMayMatchInbound(value.toObject(), inboundTag)) return true;
            }
            return false;
        }
        return true;
    }

    bool IsExactManagedMixedTerminalRule(const QJsonObject &rule, const ManagedMixedBinding &binding) {
        if (!rule.contains("inbound") || !rule.contains("outbound")) return false;
        if (rule["outbound"].toString() != binding.outboundTag) return false;

        const auto inbound = rule["inbound"];
        if (inbound.isString()) {
            if (inbound.toString() != binding.inboundTag) return false;
        } else if (inbound.isArray()) {
            const auto tags = inbound.toArray();
            if (tags.size() != 1 || !tags[0].isString() || tags[0].toString() != binding.inboundTag) return false;
        } else {
            return false;
        }

        const auto action = rule["action"].toString().trimmed().toLower();
        if (!action.isEmpty() && action != "route") return false;
        for (const auto &key: rule.keys()) {
            if (key != "inbound" && key != "outbound" && key != "action") return false;
        }
        return true;
    }

    bool RouteRuleCanTerminateOrRedirect(const QJsonObject &rule) {
        if (rule.contains("outbound")) return true;
        const auto action = rule["action"].toString().trimmed().toLower();
        return !action.isEmpty() && action != "sniff" && action != "resolve";
    }

    int ExactJsonPort(const QJsonObject &inbound) {
        const auto value = inbound["listen_port"];
        if (!value.isDouble()) return -1;
        const auto port = value.toInt(-1);
        if (value.toDouble() != static_cast<double>(port)) return -1;
        return port;
    }

    bool IsForbiddenManagedOutboundName(const QString &value) {
        static const QSet<QString> forbidden{
            "direct",
            "bypass",
            "block",
            "selector",
            "urltest",
            "url-test",
        };
        return forbidden.contains(value.trimmed().toLower());
    }

    QString CaptureManagedMixedState(const QJsonArray &inbounds, const QJsonArray &outbounds, QList<ManagedMixedBinding> &bindings) {
        QMap<QString, QList<QJsonObject>> inboundsByTag;
        for (const auto &value: inbounds) {
            if (!value.isObject()) continue;
            const auto inbound = value.toObject();
            const auto tag = inbound["tag"].toString();
            if (!tag.isEmpty()) inboundsByTag[tag] += inbound;
        }
        QMap<QString, QList<QJsonObject>> byTag;
        for (const auto &value: outbounds) {
            if (!value.isObject()) continue;
            const auto outbound = value.toObject();
            const auto tag = outbound["tag"].toString();
            if (!tag.isEmpty()) byTag[tag] += outbound;
        }

        for (auto &binding: bindings) {
            if (IsValidPort(binding.listenPort)) {
                const auto inboundMatches = inboundsByTag.value(binding.inboundTag);
                if (inboundMatches.size() != 1) {
                    return QStringLiteral("Generated managed Mixed inbound '%1' must exist exactly once; found %2.")
                        .arg(binding.inboundTag)
                        .arg(inboundMatches.size());
                }
                binding.expectedInbound = inboundMatches[0];
            } else {
                binding.expectedInbound = {};
            }
            binding.expectedOutbounds.clear();
            QString currentTag = binding.outboundTag;
            QSet<QString> visited;
            while (!currentTag.isEmpty()) {
                if (visited.contains(currentTag)) {
                    return QStringLiteral("Generated managed Mixed outbound chain for '%1' contains a detour cycle at '%2'.")
                        .arg(binding.inboundTag, currentTag);
                }
                visited.insert(currentTag);

                const auto matches = byTag.value(currentTag);
                if (matches.size() != 1) {
                    return QStringLiteral("Generated managed Mixed outbound '%1' for inbound '%2' must exist exactly once; found %3.")
                        .arg(currentTag, binding.inboundTag)
                        .arg(matches.size());
                }
                const auto outbound = matches[0];
                const auto type = outbound["type"].toString().trimmed().toLower();
                if (IsForbiddenManagedOutboundName(type)) {
                    return QStringLiteral("Generated managed Mixed outbound '%1' for inbound '%2' has forbidden type '%3'.")
                        .arg(currentTag, binding.inboundTag, type);
                }
                binding.expectedOutbounds.insert(currentTag, outbound);

                if (!outbound.contains("detour")) break;
                if (!outbound["detour"].isString()) {
                    return QStringLiteral("Generated managed Mixed outbound '%1' for inbound '%2' has a non-string detour.")
                        .arg(currentTag, binding.inboundTag);
                }
                const auto detour = outbound["detour"].toString().trimmed();
                if (detour.isEmpty()) break;
                if (IsForbiddenManagedOutboundName(detour)) {
                    return QStringLiteral("Generated managed Mixed outbound '%1' for inbound '%2' must not detour to '%3'.")
                        .arg(currentTag, binding.inboundTag, detour);
                }
                currentTag = detour;
            }
        }
        return {};
    }

    QString ValidateManagedOutboundChain(const QJsonArray &outbounds, const ManagedMixedBinding &binding) {
        QMap<QString, QList<QJsonObject>> byTag;
        for (const auto &value: outbounds) {
            if (!value.isObject()) continue;
            const auto outbound = value.toObject();
            const auto tag = outbound["tag"].toString();
            if (!tag.isEmpty()) byTag[tag] += outbound;
        }

        if (IsForbiddenManagedOutboundName(binding.outboundTag)) {
            return QStringLiteral("Managed Mixed inbound '%1' generated forbidden outbound tag '%2'.")
                .arg(binding.inboundTag, binding.outboundTag);
        }
        if (binding.expectedOutbounds.isEmpty()) {
            return QStringLiteral("Managed Mixed inbound '%1' has no captured generated outbound chain.")
                .arg(binding.inboundTag);
        }

        QString currentTag = binding.outboundTag;
        QSet<QString> visited;
        while (!currentTag.isEmpty()) {
            if (visited.contains(currentTag)) {
                return QStringLiteral("Managed Mixed outbound chain for '%1' contains a detour cycle at '%2' after custom_config merge.")
                    .arg(binding.inboundTag, currentTag);
            }
            visited.insert(currentTag);

            const auto matches = byTag.value(currentTag);
            if (matches.size() != 1) {
                return QStringLiteral("Managed Mixed outbound '%1' for inbound '%2' must exist exactly once after custom_config merge; found %3.")
                    .arg(currentTag, binding.inboundTag)
                    .arg(matches.size());
            }

            const auto outbound = matches[0];
            if (!binding.expectedOutbounds.contains(currentTag)) {
                return QStringLiteral("Managed Mixed outbound chain for '%1' was redirected to uncaptured outbound '%2' after custom_config merge.")
                    .arg(binding.inboundTag, currentTag);
            }
            if (outbound != binding.expectedOutbounds.value(currentTag)) {
                return QStringLiteral("Managed Mixed outbound '%1' for inbound '%2' changed after custom_config merge. Use the profile-level custom outbound field before line binding instead.")
                    .arg(currentTag, binding.inboundTag);
            }
            const auto type = outbound["type"].toString().trimmed().toLower();
            if (IsForbiddenManagedOutboundName(type)) {
                return QStringLiteral("Managed Mixed outbound '%1' for inbound '%2' has forbidden type '%3' after custom_config merge.")
                    .arg(currentTag, binding.inboundTag, type);
            }

            if (!outbound.contains("detour")) break;
            if (!outbound["detour"].isString()) {
                return QStringLiteral("Managed Mixed outbound '%1' for inbound '%2' has a non-string detour after custom_config merge.")
                    .arg(currentTag, binding.inboundTag);
            }

            const auto detour = outbound["detour"].toString().trimmed();
            if (detour.isEmpty()) break;
            if (IsForbiddenManagedOutboundName(detour)) {
                return QStringLiteral("Managed Mixed outbound '%1' for inbound '%2' must not detour to '%3'.")
                    .arg(currentTag, binding.inboundTag, detour);
            }
            currentTag = detour;
        }
        return {};
    }

    QString ValidateManagedMixedConfig(const QJsonObject &config, const QList<ManagedMixedBinding> &bindings) {
        if (bindings.isEmpty()) return {};

        QSet<QString> expectedTags;
        QSet<int> expectedPorts;
        for (const auto &binding: bindings) {
            if (binding.inboundTag.isEmpty() || binding.outboundTag.isEmpty() || !IsValidPort(binding.listenPort)) {
                return QStringLiteral("Internal managed Mixed definition is incomplete; refusing to start an unbound listener.");
            }
            if (expectedTags.contains(binding.inboundTag)) {
                return QStringLiteral("Internal managed Mixed definition contains duplicate inbound tag '%1'.").arg(binding.inboundTag);
            }
            if (expectedPorts.contains(binding.listenPort)) {
                return QStringLiteral("Internal managed Mixed definition contains duplicate listen port %1.").arg(binding.listenPort);
            }
            expectedTags.insert(binding.inboundTag);
            expectedPorts.insert(binding.listenPort);
        }

        if (!config["inbounds"].isArray()) {
            return QStringLiteral("Managed Mixed validation failed: custom_config removed or replaced the generated inbounds array.");
        }
        const auto inbounds = config["inbounds"].toArray();
        for (const auto &binding: bindings) {
            QList<QJsonObject> tagMatches;
            QList<QJsonObject> portMatches;
            for (const auto &value: inbounds) {
                if (!value.isObject()) continue;
                const auto inbound = value.toObject();
                if (inbound["tag"].toString() == binding.inboundTag) tagMatches += inbound;
                if (ExactJsonPort(inbound) == binding.listenPort) portMatches += inbound;
            }

            if (tagMatches.size() != 1) {
                return QStringLiteral("Managed Mixed inbound tag '%1' must exist exactly once after custom_config merge; found %2.")
                    .arg(binding.inboundTag)
                    .arg(tagMatches.size());
            }
            const auto inbound = tagMatches[0];
            if (inbound["type"].toString().trimmed().toLower() != "mixed") {
                return QStringLiteral("Managed inbound '%1' must remain type 'mixed' after custom_config merge.").arg(binding.inboundTag);
            }
            if (ExactJsonPort(inbound) != binding.listenPort) {
                return QStringLiteral("Managed Mixed inbound '%1' must retain generated listen_port %2 after custom_config merge.")
                    .arg(binding.inboundTag)
                    .arg(binding.listenPort);
            }
            if (portMatches.size() != 1 || portMatches[0]["tag"].toString() != binding.inboundTag) {
                return QStringLiteral("Managed Mixed listen port %1 is reserved for inbound '%2'; custom_config introduced a tag or port conflict.")
                    .arg(binding.listenPort)
                    .arg(binding.inboundTag);
            }
            if (binding.expectedInbound.isEmpty() || inbound != binding.expectedInbound) {
                return QStringLiteral(
                           "Managed Mixed inbound '%1' changed after custom_config merge. Its complete generated listener object must remain exact; "
                           "listener detours and other additions are forbidden.")
                    .arg(binding.inboundTag);
            }
        }

        if (!config["outbounds"].isArray()) {
            return QStringLiteral("Managed Mixed validation failed: custom_config removed or replaced the generated outbounds array.");
        }
        const auto outbounds = config["outbounds"].toArray();
        for (const auto &binding: bindings) {
            const auto outboundError = ValidateManagedOutboundChain(outbounds, binding);
            if (!outboundError.isEmpty()) return outboundError;
        }

        if (!config["route"].isObject() || !config["route"].toObject()["rules"].isArray()) {
            return QStringLiteral("Managed Mixed validation failed: custom_config removed or replaced route.rules.");
        }
        const auto rules = config["route"].toObject()["rules"].toArray();
        for (const auto &binding: bindings) {
            bool terminalFound = false;
            for (int index = 0; index < rules.size(); ++index) {
                if (!rules[index].isObject()) {
                    return QStringLiteral("Managed Mixed validation failed: route rule %1 is not an object.").arg(index);
                }
                const auto rule = rules[index].toObject();
                if (IsExactManagedMixedTerminalRule(rule, binding)) {
                    terminalFound = true;
                    break;
                }
                if (!RouteRuleMayMatchInbound(rule, binding.inboundTag)) continue;

                const auto action = rule["action"].toString().trimmed().toLower();
                if (!binding.allowResolveBeforeTerminal && action == "resolve") {
                    return QStringLiteral("Managed Mixed inbound '%1' is resolved by route rule %2 before its terminal binding; this can leak DNS outside the selected line.")
                        .arg(binding.inboundTag)
                        .arg(index);
                }
                if (RouteRuleCanTerminateOrRedirect(rule)) {
                    return QStringLiteral("Route rule %1 can redirect managed Mixed inbound '%2' before its exact binding to outbound '%3'. Move custom routing rules after the managed binding.")
                        .arg(index)
                        .arg(binding.inboundTag, binding.outboundTag);
                }
            }
            if (!terminalFound) {
                return QStringLiteral("Managed Mixed inbound '%1' has no unconditional terminal binding to generated outbound '%2' after custom_config merge.")
                    .arg(binding.inboundTag, binding.outboundTag);
            }
        }
        return {};
    }

    QString ValidateManagedResolverBindings(const QJsonObject &config, const QList<ResolverBindingRequest> &requests) {
        if (requests.isEmpty()) return {};
        if (!config["outbounds"].isArray()) {
            return QStringLiteral("Managed resolver validation failed: custom_config removed or replaced the generated outbounds array.");
        }
        if (!config["dns"].isObject() || !config["dns"].toObject()["servers"].isArray()) {
            return QStringLiteral("Managed resolver validation failed: custom_config removed or replaced dns.servers.");
        }

        QMap<QString, QList<QJsonObject>> outboundsByTag;
        for (const auto &value: config["outbounds"].toArray()) {
            if (!value.isObject()) continue;
            const auto object = value.toObject();
            outboundsByTag[object["tag"].toString()] += object;
        }
        QMap<QString, QList<QJsonObject>> dnsServersByTag;
        for (const auto &value: config["dns"].toObject()["servers"].toArray()) {
            if (!value.isObject()) continue;
            const auto object = value.toObject();
            dnsServersByTag[object["tag"].toString()] += object;
        }

        for (const auto &request: requests) {
            QStringList primaryTags;
            QMap<QString, QJsonObject> expectedDohServers;
            for (const auto &upstream: request.dohUpstreams) {
                const auto normalizedUrl = QUrl(upstream.trimmed()).toString(QUrl::FullyEncoded);
                const auto dohTag = StableRouteFluentTag("rf-doh", normalizedUrl);
                primaryTags += dohTag;
                expectedDohServers[dohTag] = BuildRouteFluentDohServer(dohTag, normalizedUrl);
            }
            const auto resolverTag = StableRouteFluentTag("rf-resolver", primaryTags.join("|"));
            const auto expectedResolver = BuildRouteFluentResolverGroup(resolverTag, primaryTags);

            const auto outboundMatches = outboundsByTag.value(request.outboundTag);
            if (outboundMatches.size() != 1) {
                return QStringLiteral("Managed server-domain resolver outbound '%1' must exist exactly once after custom_config merge; found %2.")
                    .arg(request.outboundTag)
                    .arg(outboundMatches.size());
            }
            const auto outbound = outboundMatches[0];
            if (outbound["server"].toString().trimmed() != request.server) {
                return QStringLiteral("Managed server-domain resolver outbound '%1' changed its generated server after custom_config merge.")
                    .arg(request.outboundTag);
            }
            if (!outbound["domain_resolver"].isObject() ||
                outbound["domain_resolver"].toObject() != BuildDomainResolverObject(resolverTag)) {
                return QStringLiteral("Managed server-domain resolver outbound '%1' must retain its exact strict resolver binding '%2'.")
                    .arg(request.outboundTag, resolverTag);
            }

            const auto resolverMatches = dnsServersByTag.value(resolverTag);
            if (resolverMatches.size() != 1 || resolverMatches[0] != expectedResolver) {
                return QStringLiteral("Managed resolver group '%1' must remain exactly generated, with no local/fallback fields, after custom_config merge.")
                    .arg(resolverTag);
            }
            for (auto it = expectedDohServers.constBegin(); it != expectedDohServers.constEnd(); ++it) {
                const auto serverMatches = dnsServersByTag.value(it.key());
                if (serverMatches.size() != 1 || serverMatches[0] != it.value()) {
                    return QStringLiteral("Managed DoH server '%1' must remain exactly generated after custom_config merge.")
                        .arg(it.key());
                }
            }
        }
        return {};
    }

    QString ValidateNoRouteFluentResolverFallback(const QJsonObject &config) {
        if (!config["dns"].isObject()) return {};
        // Own the value. Chaining non-const operator[] from the temporary
        // QJsonObject returned by toObject() would leave an auto-deduced
        // QJsonValueRef dangling at the end of this statement.
        const QJsonValue serversValue = config.value("dns").toObject().value("servers");
        if (serversValue.isUndefined()) return {};
        if (!serversValue.isArray()) {
            return QStringLiteral("dns.servers must be an array.");
        }

        const QSet<QString> forbiddenFields{
            "fallback",
            "fallback_enabled",
            "mode",
            "probe_domains",
            "probe_interval",
            "probe_timeout",
            "failure_threshold",
            "fallback_ttl_cap",
        };
        for (const auto &value: serversValue.toArray()) {
            if (!value.isObject()) continue;
            const auto server = value.toObject();
            if (server["type"].toString() != "routefluent_resolver_group") continue;
            for (const auto &field: forbiddenFields) {
                if (server.contains(field)) {
                    return QStringLiteral(
                               "RouteFluent resolver group '%1' contains forbidden local/fallback field '%2'. "
                               "This product only permits strict primary DoH resolution.")
                        .arg(server["tag"].toString(), field);
                }
            }
            if (!server["primary"].isArray() || server["primary"].toArray().isEmpty()) {
                return QStringLiteral("RouteFluent resolver group '%1' requires at least one strict primary resolver.")
                    .arg(server["tag"].toString());
            }
        }
        return {};
    }

    QString ValidateManagedTunConfig(const QJsonObject &config, bool expected) {
        if (config["ntp"].isObject() && config["ntp"].toObject()["write_to_system"].toBool(false)) {
            return QStringLiteral(
                "ntp.write_to_system=true is forbidden. Profile configuration may not change the Windows system clock.");
        }

        if (config["endpoints"].isArray()) {
            for (const auto &value: config["endpoints"].toArray()) {
                if (!value.isObject()) continue;
                const auto endpoint = value.toObject();
                const auto type = endpoint["type"].toString().trimmed().toLower();
                if ((type == "wireguard" && endpoint["system"].toBool(false)) ||
                    (type == "tailscale" && endpoint["system_interface"].toBool(false))) {
                    return QStringLiteral(
                               "Endpoint '%1' requests an unmanaged %2 system interface. OS network interfaces may only be changed by an explicit product-owned broker.")
                        .arg(endpoint["tag"].toString(), type);
                }
            }
        }

        if (!config["inbounds"].isArray()) {
            return expected ? QStringLiteral("Managed Tun validation failed: inbounds is not an array.") : QString();
        }

        QList<QJsonObject> tunInbounds;
        for (const auto &value: config["inbounds"].toArray()) {
            if (!value.isObject()) continue;
            const auto inbound = value.toObject();
            if (inbound["set_system_proxy"].toBool(false)) {
                return QStringLiteral(
                    "Inbound '%1' requests set_system_proxy=true. OS proxy state may only be changed by the explicit owned Windows broker, never by sing-box lifecycle.")
                    .arg(inbound["tag"].toString());
            }
            if (inbound["type"].toString().trimmed().toLower() == "tun") tunInbounds += inbound;
        }
        if (!expected) {
            return tunInbounds.isEmpty()
                       ? QString()
                       : QStringLiteral("A Tun inbound is present without an explicit product Tun enable action.");
        }
        if (tunInbounds.size() != 1) {
            return QStringLiteral("Managed internal Tun requires exactly one Tun inbound; found %1.").arg(tunInbounds.size());
        }

        const auto tun = tunInbounds[0];
        const QJsonObject expectedTun{
            {"tag", "tun-in"},
            {"type", "tun"},
            {"interface_name", genTunName()},
            {"auto_route", true},
            {"endpoint_independent_nat", true},
            {"mtu", dataStore->vpn_mtu},
            {"stack", Preset::SingBox::VpnImplementation.value(dataStore->vpn_implementation)},
            {"strict_route", true},
            {"address", QJsonArray{"172.19.0.1/28", "fdfe:dcba:9876::1/126"}},
        };
        // Exact equality is deliberate.  Fields such as route_address,
        // route_exclude_address and platform.http_proxy can turn an otherwise
        // strict-looking Tun inbound into a bypass or another OS side effect.
        // Custom configuration must not add, remove or replace any field on
        // the product-owned Tun listener.
        if (tun != expectedTun) {
            return QStringLiteral(
                "Managed internal Tun was changed after custom_config merge. Its complete field set must remain exactly product-generated; "
                "route/include/exclude/platform overrides and all other additions are forbidden.");
        }
        if (!config["route"].isObject() ||
            !config["route"].toObject()["auto_detect_interface"].isBool() ||
            !config["route"].toObject()["auto_detect_interface"].toBool()) {
            return QStringLiteral("Managed internal Tun requires the generated route interface auto-detection policy.");
        }
        const auto route = config["route"].toObject();
        if (route.contains("default_interface")) {
            return QStringLiteral(
                "Managed internal Tun forbids route.default_interface because it overrides the generated interface auto-detection policy.");
        }

        const QSet<QString> forbiddenBindFields{
            "bind_interface",
            "inet4_bind_address",
            "inet6_bind_address",
        };
        auto validateNoBindOverride = [&](const QJsonArray &objects, const QString &section) -> QString {
            for (const auto &value: objects) {
                if (!value.isObject()) continue;
                const auto object = value.toObject();
                for (const auto &field: forbiddenBindFields) {
                    if (object.contains(field)) {
                        auto objectName = object["tag"].toString();
                        if (objectName.isEmpty()) objectName = object["action"].toString();
                        if (objectName.isEmpty()) objectName = QStringLiteral("<unnamed>");
                        return QStringLiteral(
                                   "Managed internal Tun forbids %1.%2 on '%3' because it disables the generated interface auto-detection policy.")
                            .arg(section, field, objectName);
                    }
                }
            }
            return {};
        };
        auto bindError = validateNoBindOverride(config["outbounds"].toArray(), "outbounds");
        if (bindError.isEmpty()) bindError = validateNoBindOverride(config["endpoints"].toArray(), "endpoints");
        if (bindError.isEmpty() && config["dns"].isObject()) {
            bindError = validateNoBindOverride(config["dns"].toObject()["servers"].toArray(), "dns.servers");
        }
        if (bindError.isEmpty() && config["ntp"].isObject()) {
            bindError = validateNoBindOverride(QJsonArray{config["ntp"]}, "ntp");
        }
        const auto validateRouteRuleBindOverrides =
            [&](auto&& self, const QJsonArray& rules, const QString& section) -> QString {
            auto error = validateNoBindOverride(rules, section);
            if (!error.isEmpty()) return error;
            for (int index = 0; index < rules.size(); ++index) {
                if (!rules[index].isObject()) continue;
                const auto nestedRules = rules[index].toObject()["rules"];
                if (!nestedRules.isArray()) continue;
                error = self(
                    self,
                    nestedRules.toArray(),
                    QStringLiteral("%1[%2].rules").arg(section).arg(index));
                if (!error.isEmpty()) return error;
            }
            return {};
        };
        if (bindError.isEmpty()) {
            bindError = validateRouteRuleBindOverrides(
                validateRouteRuleBindOverrides,
                route["rules"].toArray(),
                QStringLiteral("route.rules"));
        }
        if (!bindError.isEmpty()) return bindError;
        return {};
    }

    // Common

    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport) {
        auto result = std::make_shared<BuildConfigResult>();
        auto status = std::make_shared<BuildConfigStatus>();
        status->ent = ent;
        status->result = result;
        status->forTest = forTest;
        status->forExport = forExport;
        const auto managedInternalTunExpected =
            !forTest && !forExport && dataStore->vpn_internal_tun && dataStore->spmode_vpn;

        if (!forTest && !forExport && !dataStore->aux_profile_ports_load_error.isEmpty()) {
            result->error = dataStore->aux_profile_ports_load_error;
            return result;
        }

        auto customBean = dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get());
        const auto isInternalFull = customBean != nullptr && customBean->core == "internal-full";
        if (isInternalFull) {
            result->coreConfig = QString2QJsonObject(customBean->config_simple);
            if (forTest) {
                result->error = QStringLiteral(
                    "internal-full profiles cannot be launched by latency/full-test because their listeners and services are not a bounded test surface.");
            }
            if (!forTest && !forExport && dataStore->spmode_vpn) {
                result->error = QStringLiteral(
                    "internal-full profiles cannot run under the product Tun switch because their OS-network state is not managed or observable. "
                    "Disable Tun explicitly or use a standard profile.");
            }
            if (!forTest && !forExport && !dataStore->aux_profile_ports.isEmpty()) {
                result->error = QStringLiteral(
                    "internal-full profiles are isolated full configurations and cannot run with auxiliary Mixed lines. "
                    "Select a standard profile or disable all auxiliary Mixed mappings.");
            }
        } else {
            BuildConfigSingBox(status);
        }

        // A test creates and starts a temporary Box.  Only the bounded
        // generator output may enter that process; top-level custom_config can
        // otherwise add listeners/services or replace the tested chain with
        // direct. Profile-level custom_outbound was already applied and
        // validated while the chain was generated.
        const auto generatedCoreConfig = result->coreConfig;
        MergeJson(result->coreConfig, QString2QJsonObject(ent->bean->custom_config));
        if (forTest && result->error.isEmpty() && result->coreConfig != generatedCoreConfig) {
            result->error = QStringLiteral(
                "Latency/full-test refuses top-level custom_config changes because the temporary core must remain an exact bounded generated configuration.");
        }

        if (result->error.isEmpty()) {
            result->error = ValidateNoRouteFluentResolverFallback(result->coreConfig);
        }
        if (result->error.isEmpty()) {
            result->error = ValidateManagedTunConfig(
                result->coreConfig,
                managedInternalTunExpected);
        }

        // custom_config is intentionally powerful, but it must not be able to
        // silently detach a managed Mixed port from the line selected by the
        // standard generator. Full configurations remain isolated above.
        if (!isInternalFull && result->error.isEmpty()) {
            result->error = ValidateManagedMixedConfig(result->coreConfig, status->managedMixedBindings);
        }
        if (!isInternalFull && result->error.isEmpty()) {
            result->error = ValidateManagedResolverBindings(result->coreConfig, status->resolverBindingRequests);
        }

        if (result->error.isEmpty()) {
            result->managedInternalTun = managedInternalTunExpected;
        }

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
        if (ents.isEmpty()) {
            status->result->error = QStringLiteral("chain has no profiles");
            return {};
        }

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
        QString chainTagOut = BuildChainInternal(chainId, ents, status);

        // Chain and auxiliary ent traffic stat
        if (ents.length() > 1 || chainId != 0) {
            status->result->outboundStats += NekoGui_traffic::TrafficBinding{
                status->ent->id,
                chainTagOut.toStdString(),
                status->ent->traffic_data,
            };
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
                auto replaced = status->outbounds.last().toObject();
                replaced["detour"] = tagOut;
                status->outbounds.removeLast();
                status->outbounds += replaced;
            } else {
                // index == 0 means last profile in chain / not chain
                chainTagOut = tagOut;
                if (chainId == 0) status->result->outboundStat = ent->traffic_data;
            }

            if (globalAlreadyBuilt) {
                index++;
                continue;
            }

            if (needGlobal) {
                status->globalProfiles += ent->id;
            }

            // Outbound

            QJsonObject outbound;
            auto stream = GetStreamSettings(ent->bean.get());

            const auto coreR = ent->bean->BuildCoreObjSingBox();
            if (!coreR.error.isEmpty()) { // rejected
                status->result->error = coreR.error;
                return {};
            }
            if (coreR.outbound.isEmpty()) {
                status->result->error = "unsupported outbound";
                return {};
            }
            outbound = coreR.outbound;

            ApplySubscriptionAnyTLSClientDefault(ent, outbound);

            // outbound misc
            outbound["tag"] = tagOut;
            status->result->outboundStats += NekoGui_traffic::TrafficBinding{
                ent->id,
                tagOut.toStdString(),
                ent->traffic_data,
            };

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
            if (ent->type == "vless" && !outbound.value("flow").toString().isEmpty()) {
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
            // QJsonObject::operator[] on a non-const object inserts a null
            // value for a missing key, and auto would retain a QJsonValueRef.
            // Either behavior corrupts the generated snapshot and can make a
            // later detour rewrite compare equal to itself.  Capture an owned,
            // non-mutating value and the original key presence instead.
            const bool generatedHadDetour = outbound.contains("detour");
            const QJsonValue generatedDetour = outbound.value("detour");
            MergeJson(outbound, QString2QJsonObject(ent->bean->custom_outbound));
            if (outbound.contains("detour") != generatedHadDetour ||
                (generatedHadDetour && outbound.value("detour") != generatedDetour)) {
                status->result->error = QStringLiteral(
                    "Profile-level custom outbound settings may not add or change detour. Managed chain detours are generated exclusively from the selected profile/front-proxy chain.");
                return {};
            }

            const auto outboundServer = outbound.value("server").toString().trimmed();
            const auto resolverDohUpstreams = ParseResolverDohUpstreams(EffectiveResolverDohUpstreams(ent));
            if (!outboundServer.isEmpty() && !IsIpAddress(outboundServer) && !resolverDohUpstreams.isEmpty()) {
                status->resolverBindingRequests += ResolverBindingRequest{
                    static_cast<int>(status->outbounds.size()),
                    tagOut,
                    outboundServer,
                    resolverDohUpstreams,
                };
            }

            status->outbounds += outbound;
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
        if (!status->forTest) {
            if (!IsValidPort(dataStore->inbound_socks_port)) {
                status->result->error = QStringLiteral("Mixed listen port must be between 1 and 65535.");
                return;
            }
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
            // Preserve the client destination name until the exact main-line
            // binding. Resolving it through the global/default DNS path before
            // that binding would violate the same isolation contract as an
            // auxiliary line.
            AppendInboundRouteActions(status->frontRoutingRules, "mixed-in", false);
        }

        // tun-in
        if (dataStore->vpn_internal_tun && dataStore->spmode_vpn && !status->forTest && !status->forExport) {
            QJsonObject inboundObj;
            inboundObj["tag"] = "tun-in";
            inboundObj["type"] = "tun";
            inboundObj["interface_name"] = genTunName();
            inboundObj["auto_route"] = true;
            inboundObj["endpoint_independent_nat"] = true;
            inboundObj["mtu"] = dataStore->vpn_mtu;
            inboundObj["stack"] = Preset::SingBox::VpnImplementation.value(dataStore->vpn_implementation);
            // Windows product TUN is fail-closed: cover both address families
            // and enforce strict routing regardless of legacy persisted UI
            // values. Crash/restart windows still require the persistent WFP
            // runtime described in ADR 0008.
            inboundObj["strict_route"] = true;
            QJsonArray tunAddress{"172.19.0.1/28", "fdfe:dcba:9876::1/126"};
            inboundObj["address"] = tunAddress;
            status->inbounds += inboundObj;
            AppendInboundRouteActions(status, "tun-in");
        }

        // Outbounds
        auto tagProxy = BuildChain(0, status);
        if (!status->result->error.isEmpty()) return;
        if (tagProxy.isEmpty()) {
            status->result->error = QStringLiteral("The primary profile produced an empty outbound chain.");
            return;
        }

        // A Mixed listener is a line selector, not a generic routing ingress.
        // Bind the primary port to the selected primary chain before any user
        // domain/IP routing rules, matching the existing auxiliary-port model.
        if (!status->forTest) {
            status->managedMixedBindings += ManagedMixedBinding{
                "mixed-in",
                dataStore->inbound_socks_port,
                tagProxy,
                false,
            };
            status->frontRoutingRules += QJsonObject{
                {"inbound", QJsonArray{"mixed-in"}},
                {"outbound", tagProxy},
            };
        }

        if (!status->forTest && !status->forExport) {
            int auxChainId = 1000;
            const auto mainEnt = status->ent;
            QSet<int> managedMixedPorts{dataStore->inbound_socks_port};
            for (auto it = dataStore->aux_profile_ports.constBegin(); it != dataStore->aux_profile_ports.constEnd(); ++it) {
                auto auxProfile = profileManager->GetProfile(it.key());
                if (auxProfile == nullptr || auxProfile->bean == nullptr) {
                    status->result->error = QStringLiteral("Auxiliary Mixed profile %1 does not exist.").arg(it.key());
                    return;
                }
                if (status->ent != nullptr && auxProfile->id == status->ent->id) {
                    status->result->error = QStringLiteral("The main profile cannot also be an auxiliary Mixed line.");
                    return;
                }
                auto auxGroup = profileManager->GetGroup(auxProfile->gid);
                if (auxGroup == nullptr || auxGroup->archive) {
                    status->result->error = QStringLiteral("Auxiliary Mixed profile %1 is in a missing or archived group.").arg(auxProfile->id);
                    return;
                }
                if (!IsValidPort(it.value())) {
                    status->result->error = QStringLiteral("Auxiliary Mixed port for profile %1 must be between 1 and 65535.").arg(auxProfile->id);
                    return;
                }
                if (managedMixedPorts.contains(it.value())) {
                    status->result->error = QStringLiteral("Duplicate managed Mixed port: %1.").arg(it.value());
                    return;
                }

                auto auxCustomBean = dynamic_cast<NekoGui_fmt::CustomBean *>(auxProfile->bean.get());
                if (auxCustomBean != nullptr && auxCustomBean->core == "internal-full") {
                    status->result->error = QStringLiteral(
                        "Auxiliary Mixed profile %1 uses internal-full and cannot be embedded in a managed concurrent-line configuration.")
                                                .arg(auxProfile->id);
                    return;
                }

                const auto inboundTag = QStringLiteral("aux-mixed-%1").arg(auxProfile->id);
                status->ent = auxProfile;
                const auto auxOutboundTag = BuildChain(auxChainId++, status);
                status->ent = mainEnt;
                if (!status->result->error.isEmpty()) return;
                if (auxOutboundTag.isEmpty()) {
                    status->result->error = QStringLiteral("Auxiliary Mixed profile %1 produced an empty outbound chain.").arg(auxProfile->id);
                    return;
                }

                // Commit the listener only after its complete outbound chain
                // was built. This prevents an orphan Mixed port from falling
                // through to route.final when an auxiliary chain is invalid.
                status->inbounds += QJsonObject{
                    {"tag", inboundTag},
                    {"type", "mixed"},
                    {"listen", "127.0.0.1"},
                    {"listen_port", it.value()},
                };
                // An auxiliary line must not resolve client destination names
                // through the main/default DNS path before its terminal route.
                // Preserve the domain for the bound proxy chain instead.
                AppendInboundRouteActions(status->frontRoutingRules, inboundTag, false);

                status->frontRoutingRules += QJsonObject{
                    {"inbound", QJsonArray{inboundTag}},
                    {"outbound", auxOutboundTag},
                };
                status->managedMixedBindings += ManagedMixedBinding{
                    inboundTag,
                    it.value(),
                    auxOutboundTag,
                    false,
                };
                managedMixedPorts.insert(it.value());
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
        if (dataStore->fake_dns && dataStore->vpn_internal_tun && dataStore->spmode_vpn && !status->forTest && !status->forExport) {
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
        if (dataStore->fake_dns && dataStore->vpn_internal_tun && dataStore->spmode_vpn && !status->forTest && !status->forExport) {
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
            dnsServers = dns.value("servers").toArray();
        }
        ApplyRouteFluentResolverBindings(status, dnsServers);
        if (!status->result->error.isEmpty()) return;
        if (status->forTest) {
            QList<ManagedMixedBinding> testBindings{
                ManagedMixedBinding{QStringLiteral("isolated-test-profile"), -1, tagProxy, false},
            };
            status->result->error = CaptureManagedMixedState(status->inbounds, status->outbounds, testBindings);
        } else {
            status->result->error = CaptureManagedMixedState(status->inbounds, status->outbounds, status->managedMixedBindings);
        }
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
        if (dataStore->vpn_internal_tun && dataStore->spmode_vpn && !status->forTest && !status->forExport) {
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

        }

        // Geo assets are a live-routing dependency only. Bounded tests remove
        // all user/geosite rules, while exports intentionally omit machine-
        // local asset paths; neither path should fail merely because the GUI
        // build directory does not contain release assets.
        QString geoip;
        QString geosite;
        if (!status->forTest && !status->forExport) {
            geoip = FindCoreAsset("geoip.db");
            geosite = FindCoreAsset("geosite.db");
            if (geoip.isEmpty()) status->result->error = +"geoip.db not found";
            if (geosite.isEmpty()) status->result->error = +"geosite.db not found";
        }

        // final add routing rule
        auto routingRules = status->frontRoutingRules;
        auto profileRoutingRules = NormalizeRouteRuleActions(QString2QJsonObject(dataStore->routing->custom)["rules"].toArray());
        if (status->forTest) {
            routingRules = {};
            profileRoutingRules = {};
        } else {
            QJSONARRAY_ADD(routingRules, profileRoutingRules)
        }
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
            // Preserve NekoRay's product-TUN behavior (internal or companion
            // process), including its bounded test configuration semantics.
            // Mixed-only mode follows the Windows routing table. Test-machine
            // or dual-NekoRay exceptions must never be encoded here as an
            // unconditional interface-selection policy. Export still removes
            // this process-local field below, as it did upstream.
            {"auto_detect_interface", dataStore->spmode_vpn}};
        if (!status->forTest && !status->forExport) {
            routeObj["geoip"] = QJsonObject{{"path", geoip}};
            routeObj["geosite"] = QJsonObject{{"path", geosite}};
        }
        if (!status->forTest) {
            routeObj["final"] = finalOutbound;
        }
        if (!dataStore->routing->use_dns_object) {
            routeObj["default_domain_resolver"] = QJsonObject{{"server", "dns-direct"}};
        }
        if (status->forExport) {
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
