#include "MultiMapperExport.hpp"

#include "fmt/includes.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#ifndef NKR_NO_YAML
#include <yaml-cpp/yaml.h>
#endif

namespace NekoGui_sub {
namespace {
    constexpr auto kMihomoAnyTLSClient = "mihomo/1.19.28";

    struct ClientSetting {
        QString mode;
        QString value;

        [[nodiscard]] bool isRuntimeValue() const {
            return !mode.isEmpty() && mode != "native" && !value.trimmed().isEmpty();
        }
    };

    QStringList splitDohLines(const QString &raw) {
        QString normalized = raw;
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

    QStringList splitCommaValues(const QString &raw) {
        QStringList out;
        QSet<QString> seen;
        for (auto item: raw.split(",", Qt::SkipEmptyParts)) {
            item = item.trimmed();
            if (item.isEmpty() || seen.contains(item)) continue;
            seen.insert(item);
            out << item;
        }
        return out;
    }

    void appendUnique(QStringList *target, const QStringList &values) {
        if (target == nullptr) return;
        for (const auto &value: values) {
            if (!value.trimmed().isEmpty() && !target->contains(value)) *target << value;
        }
    }

    QJsonObject parseSubscriptionInfo(const QString &raw) {
        QJsonObject out;
        const auto parts = raw.split(";", Qt::SkipEmptyParts);
        for (const auto &part: parts) {
            const auto kv = part.split("=", Qt::KeepEmptyParts);
            if (kv.size() != 2) continue;
            const auto key = kv[0].trimmed().toLower();
            bool ok = false;
            const auto value = kv[1].trimmed().toLongLong(&ok);
            if (!ok) continue;
            if (QStringList{"upload", "download", "total", "expire"}.contains(key)) {
                out[key] = QString::number(value);
            }
        }
        return out;
    }

    QString effectiveSourceType(const std::shared_ptr<NekoGui::Group> &group) {
        if (group == nullptr) return "manual";
        const auto sourceType = group->source_type.trimmed().toLower();
        if (!sourceType.isEmpty()) return sourceType;
        return group->url.trimmed().isEmpty() ? "manual" : "subscription";
    }

    ClientSetting normalizeClientSetting(QString mode, QString value, const QString &sourceType = {}) {
        mode = mode.trimmed().toLower();
        value = value.trimmed();
        if (mode.isEmpty() && sourceType == "clash") {
            mode = "mihomo";
            value = kMihomoAnyTLSClient;
        }
        if (mode == "mihomo") {
            return {mode, value.isEmpty() ? QString::fromLatin1(kMihomoAnyTLSClient) : value};
        }
        if (mode == "custom") {
            return value.isEmpty() ? ClientSetting{} : ClientSetting{mode, value};
        }
        return {};
    }

    ClientSetting groupClientDefault(const std::shared_ptr<NekoGui::Group> &group) {
        if (group == nullptr) return {};
        return normalizeClientSetting(group->default_client_mode, group->default_client_value, effectiveSourceType(group));
    }

    ClientSetting profileExplicitAnyTLSClient(const NekoGui_fmt::AnyTLSBean *bean) {
        if (bean == nullptr) return {};
        if (bean->inheritSubscriptionClient) return {};
        return normalizeClientSetting(bean->anytlsClientMode, bean->anytlsClientValue);
    }

    ClientSetting profileEffectiveAnyTLSClient(const NekoGui_fmt::AnyTLSBean *bean,
                                               const std::shared_ptr<NekoGui::Group> &group) {
        if (bean == nullptr) return {};
        if (bean->inheritSubscriptionClient) return groupClientDefault(group);
        return profileExplicitAnyTLSClient(bean);
    }

    bool sameClientSetting(const ClientSetting &a, const ClientSetting &b) {
        return a.mode == b.mode && a.value == b.value;
    }

    QJsonObject groupDefaults(const std::shared_ptr<NekoGui::Group> &group) {
        QJsonObject defaults;
        const auto client = groupClientDefault(group);
        if (client.isRuntimeValue()) {
            defaults["client"] = QJsonObject{
                {"mode", client.mode},
                {"value", client.value},
            };
        }

        const auto dohs = splitDohLines(group == nullptr ? QString{} : group->default_server_resolver_doh);
        if (!dohs.isEmpty()) {
            defaults["server_resolver"] = QJsonObject{
                {"mode", "doh"},
                {"doh_nameservers", QList2QJsonArray(dohs)},
                {"fallback", group->default_server_resolver_allow_local_fallback ? "local" : "strict"},
            };
        }
        return defaults;
    }

    QJsonObject streamSettingsToMultiMapper(NekoGui_fmt::SingBoxTransportSettings *stream) {
        if (stream == nullptr) return {};
        QJsonObject streamObj{
            {"network", stream->network},
            {"security", stream->security},
        };
        QJsonObject netSettings;
        if (!stream->path.isEmpty()) netSettings["path"] = stream->path;
        if (!stream->host.isEmpty()) {
            netSettings["headers"] = QJsonObject{{"Host", stream->host}};
            netSettings["host"] = QJsonArray{stream->host};
        }
        if (!stream->header_type.isEmpty()) netSettings["header"] = QJsonObject{{"type", stream->header_type}};
        if (!stream->network.isEmpty()) streamObj[stream->network + "Settings"] = netSettings;

        QJsonObject tlsSettings;
        if (!stream->sni.isEmpty()) tlsSettings["serverName"] = stream->sni;
        if (stream->allow_insecure) tlsSettings["allowInsecure"] = true;
        if (!stream->alpn.isEmpty()) tlsSettings["alpn"] = QList2QJsonArray(splitCommaValues(stream->alpn));
        if (!stream->utlsFingerprint.isEmpty()) tlsSettings["fingerprint"] = stream->utlsFingerprint;
        if (!stream->reality_pbk.isEmpty()) tlsSettings["publicKey"] = stream->reality_pbk;
        if (!stream->reality_sid.isEmpty()) tlsSettings["shortId"] = stream->reality_sid;
        if (!stream->security.isEmpty()) streamObj[stream->security + "Settings"] = tlsSettings;
        return streamObj;
    }

    QJsonObject buildItem(const std::shared_ptr<NekoGui::ProxyEntity> &profile,
                          const std::shared_ptr<NekoGui::Group> &group) {
        QJsonObject item{
            {"protocol", profile->type},
            {"address", profile->bean->serverAddress},
            {"port", profile->bean->serverPort},
            {"tag", profile->bean->name},
        };

        const auto groupDoh = group == nullptr ? QString{} : group->default_server_resolver_doh.trimmed();
        QJsonObject inherit{
            {"client", profile->bean->inheritSubscriptionClient},
            {"server_resolver", profile->bean->inheritSubscriptionResolver && profile->bean->serverResolverDohUpstreams.trimmed().isEmpty()},
        };
        item["inherit"] = inherit;
        if (!profile->bean->serverResolverDohUpstreams.trimmed().isEmpty() &&
            profile->bean->serverResolverDohUpstreams.trimmed() != groupDoh) {
            item["server_resolver_override"] = QJsonObject{
                {"mode", "doh"},
                {"doh_nameservers", QList2QJsonArray(splitDohLines(profile->bean->serverResolverDohUpstreams))},
                {"fallback", profile->bean->serverResolverAllowLocalFallback ? "local" : "strict"},
            };
        }

        if (profile->type == "anytls") {
            const auto bean = profile->AnyTLSBean();
            item["settings"] = QJsonObject{{"servers", QJsonArray{QJsonObject{
                {"address", bean->serverAddress},
                {"port", bean->serverPort},
                {"password", bean->password},
            }}}};
            QJsonObject tls{{"show", false}};
            if (!bean->sni.isEmpty()) tls["serverName"] = bean->sni;
            if (bean->allowInsecure) tls["allowInsecure"] = true;
            if (!bean->alpn.isEmpty()) tls["alpn"] = QList2QJsonArray(splitCommaValues(bean->alpn));
            if (!bean->utlsFingerprint.isEmpty()) tls["fingerprint"] = bean->utlsFingerprint;
            if (!bean->realityPublicKey.isEmpty()) tls["publicKey"] = bean->realityPublicKey;
            if (!bean->realityShortId.isEmpty()) tls["shortId"] = bean->realityShortId;
            item["streamSettings"] = QJsonObject{
                {"network", "tcp"},
                {"security", "tls"},
                {"tlsSettings", tls},
            };
            QJsonObject anytlsSettings;
            if (!bean->idleSessionCheckInterval.isEmpty()) anytlsSettings["idle_session_check_interval"] = bean->idleSessionCheckInterval;
            if (!bean->idleSessionTimeout.isEmpty()) anytlsSettings["idle_session_timeout"] = bean->idleSessionTimeout;
            if (bean->minIdleSession > 0) anytlsSettings["min_idle_session"] = bean->minIdleSession;
            if (!anytlsSettings.isEmpty()) item["anytlsSettings"] = anytlsSettings;

            const auto explicitClient = profileExplicitAnyTLSClient(bean);
            if (explicitClient.isRuntimeValue() && !sameClientSetting(explicitClient, groupClientDefault(group))) {
                item["client_override"] = QJsonObject{
                    {"mode", explicitClient.mode},
                    {"value", explicitClient.value},
                };
            }
        } else if (profile->type == "trojan") {
            const auto bean = profile->TrojanVLESSBean();
            item["settings"] = QJsonObject{{"servers", QJsonArray{QJsonObject{
                {"address", bean->serverAddress},
                {"port", bean->serverPort},
                {"password", bean->password},
            }}}};
            item["streamSettings"] = streamSettingsToMultiMapper(bean->stream.get());
        } else if (profile->type == "vless") {
            const auto bean = profile->TrojanVLESSBean();
            QJsonObject user{
                {"id", bean->password},
                {"encryption", "none"},
            };
            if (!bean->flow.isEmpty()) user["flow"] = bean->flow;
            item["settings"] = QJsonObject{{"vnext", QJsonArray{QJsonObject{
                {"address", bean->serverAddress},
                {"port", bean->serverPort},
                {"users", QJsonArray{user}},
            }}}};
            item["streamSettings"] = streamSettingsToMultiMapper(bean->stream.get());
        } else if (profile->type == "vmess") {
            const auto bean = profile->VMessBean();
            item["settings"] = QJsonObject{{"vnext", QJsonArray{QJsonObject{
                {"address", bean->serverAddress},
                {"port", bean->serverPort},
                {"users", QJsonArray{QJsonObject{
                    {"id", bean->uuid},
                    {"alterId", bean->aid},
                    {"security", bean->security},
                }}},
            }}}};
            item["streamSettings"] = streamSettingsToMultiMapper(bean->stream.get());
        } else if (profile->type == "shadowsocks") {
            const auto bean = profile->ShadowSocksBean();
            item["settings"] = QJsonObject{{"servers", QJsonArray{QJsonObject{
                {"address", bean->serverAddress},
                {"port", bean->serverPort},
                {"method", bean->method},
                {"password", bean->password},
            }}}};
        } else if (profile->type == "socks" || profile->type == "http") {
            const auto bean = profile->SocksHTTPBean();
            QJsonObject server{
                {"address", bean->serverAddress},
                {"port", bean->serverPort},
            };
            if (!bean->username.isEmpty()) server["username"] = bean->username;
            if (!bean->password.isEmpty()) server["password"] = bean->password;
            item["settings"] = QJsonObject{{"servers", QJsonArray{server}}};
            item["streamSettings"] = streamSettingsToMultiMapper(bean->stream.get());
        }

        item["nekoray"] = QJsonObject{{"profile_id", profile->id}};
        return item;
    }

    QString groupBaseKey(const std::shared_ptr<NekoGui::Group> &group) {
        if (group == nullptr) return {};
        const auto name = group->name.trimmed();
        return name.isEmpty() ? QStringLiteral("group-%1").arg(group->id) : name;
    }

    QString ensureGroupKey(const std::shared_ptr<NekoGui::Group> &group,
                           QMap<int, QString> *keysByGroup,
                           QSet<QString> *usedKeys) {
        if (group == nullptr || keysByGroup == nullptr || usedKeys == nullptr) return {};
        if (keysByGroup->contains(group->id)) return keysByGroup->value(group->id);
        const auto base = groupBaseKey(group);
        auto key = base;
        int suffix = 2;
        while (usedKeys->contains(key)) key = base + QStringLiteral(" (%1)").arg(suffix++);
        usedKeys->insert(key);
        (*keysByGroup)[group->id] = key;
        return key;
    }

#ifndef NKR_NO_YAML
    std::string utf8(const QString &value) {
        return value.toUtf8().toStdString();
    }

    void emitString(YAML::Emitter &out, const QString &value) {
        out << YAML::DoubleQuoted << utf8(value);
    }

    void emitStringField(YAML::Emitter &out, const char *key, const QString &value, bool skipEmpty = true) {
        if (skipEmpty && value.trimmed().isEmpty()) return;
        out << YAML::Key << key << YAML::Value;
        emitString(out, value);
    }

    void emitIntField(YAML::Emitter &out, const char *key, int value) {
        out << YAML::Key << key << YAML::Value << value;
    }

    void emitBoolField(YAML::Emitter &out, const char *key, bool value, bool skipFalse = true) {
        if (skipFalse && !value) return;
        out << YAML::Key << key << YAML::Value << value;
    }

    void emitStringListField(YAML::Emitter &out, const char *key, const QStringList &values) {
        if (values.isEmpty()) return;
        out << YAML::Key << key << YAML::Value << YAML::BeginSeq;
        for (const auto &value: values) emitString(out, value);
        out << YAML::EndSeq;
    }

    void emitDohList(YAML::Emitter &out, const QStringList &values) {
        out << YAML::BeginSeq;
        for (const auto &value: values) emitString(out, value);
        out << YAML::EndSeq;
    }

    void emitSubscriptionInfo(YAML::Emitter &out, const QJsonObject &subInfo) {
        if (subInfo.isEmpty()) return;
        out << YAML::Key << "subscription_info" << YAML::Value << YAML::BeginMap;
        for (const auto &key: subInfo.keys()) {
            out << YAML::Key << utf8(key) << YAML::Value;
            emitString(out, subInfo.value(key).toString());
        }
        out << YAML::EndMap;
    }

    void emitResolverObject(YAML::Emitter &out,
                            const QStringList &dohs,
                            bool allowLocalFallback,
                            const char *modeKey = "mode",
                            const char *dohKey = "doh_nameservers",
                            const char *fallbackKey = "fallback") {
        out << YAML::BeginMap;
        emitStringField(out, modeKey, "doh", false);
        out << YAML::Key << dohKey << YAML::Value;
        emitDohList(out, dohs);
        emitStringField(out, fallbackKey, allowLocalFallback ? "local" : "strict", false);
        out << YAML::EndMap;
    }

    void emitTransport(YAML::Emitter &out, const NekoGui_fmt::SingBoxTransportSettings *stream) {
        if (stream == nullptr) return;
        const auto network = stream->network.trimmed();
        if (!network.isEmpty() && network != "tcp") emitStringField(out, "network", network, false);
        if (stream->security == "tls") emitBoolField(out, "tls", true, false);
        emitStringField(out, "sni", stream->sni);
        emitBoolField(out, "skip-cert-verify", stream->allow_insecure);
        emitStringListField(out, "alpn", splitCommaValues(stream->alpn));
        emitStringField(out, "client-fingerprint", stream->utlsFingerprint);

        if (network == "ws") {
            out << YAML::Key << "ws-opts" << YAML::Value << YAML::BeginMap;
            emitStringField(out, "path", stream->path);
            if (!stream->host.isEmpty()) {
                out << YAML::Key << "headers" << YAML::Value << YAML::BeginMap;
                emitStringField(out, "Host", stream->host, false);
                out << YAML::EndMap;
            }
            if (stream->ws_early_data_length > 0) emitIntField(out, "max-early-data", stream->ws_early_data_length);
            emitStringField(out, "early-data-header-name", stream->ws_early_data_name);
            out << YAML::EndMap;
        } else if (network == "grpc") {
            out << YAML::Key << "grpc-opts" << YAML::Value << YAML::BeginMap;
            emitStringField(out, "grpc-service-name", stream->path);
            out << YAML::EndMap;
        } else if (network == "http" || network == "h2") {
            out << YAML::Key << "h2-opts" << YAML::Value << YAML::BeginMap;
            emitStringListField(out, "host", splitCommaValues(stream->host));
            emitStringField(out, "path", stream->path);
            out << YAML::EndMap;
        } else if (stream->header_type == "http") {
            out << YAML::Key << "http-opts" << YAML::Value << YAML::BeginMap;
            emitStringListField(out, "path", splitCommaValues(stream->path));
            out << YAML::Key << "headers" << YAML::Value << YAML::BeginMap;
            emitStringListField(out, "Host", splitCommaValues(stream->host));
            out << YAML::EndMap;
            out << YAML::EndMap;
        }

        if (!stream->reality_pbk.trimmed().isEmpty() || !stream->reality_sid.trimmed().isEmpty()) {
            out << YAML::Key << "reality-opts" << YAML::Value << YAML::BeginMap;
            emitStringField(out, "public-key", stream->reality_pbk);
            emitStringField(out, "short-id", stream->reality_sid);
            out << YAML::EndMap;
        }
    }

    void emitBaseProxyFields(YAML::Emitter &out,
                             const std::shared_ptr<NekoGui::ProxyEntity> &profile,
                             const QString &clashType,
                             const QString &groupKey) {
        emitStringField(out, "name", profile->bean->name, false);
        emitStringField(out, "type", clashType, false);
        emitStringField(out, "server", profile->bean->serverAddress, false);
        emitIntField(out, "port", profile->bean->serverPort);
        emitStringField(out, "x-source-tag", groupKey, false);
        emitIntField(out, "x-nekoray-profile-id", profile->id);

        out << YAML::Key << "x-inherit" << YAML::Value << YAML::BeginMap;
        emitBoolField(out, "client", profile->bean->inheritSubscriptionClient, false);
        emitBoolField(out,
                      "server-resolver",
                      profile->bean->inheritSubscriptionResolver &&
                          profile->bean->serverResolverDohUpstreams.trimmed().isEmpty(),
                      false);
        out << YAML::EndMap;
    }

    void emitServerResolverOverride(YAML::Emitter &out,
                                    const std::shared_ptr<NekoGui::ProxyEntity> &profile,
                                    const std::shared_ptr<NekoGui::Group> &group) {
        const auto profileDoh = profile->bean->serverResolverDohUpstreams.trimmed();
        const auto groupDoh = group == nullptr ? QString{} : group->default_server_resolver_doh.trimmed();
        if (profileDoh.isEmpty() || profileDoh == groupDoh) return;
        out << YAML::Key << "x-server-resolver" << YAML::Value;
        emitResolverObject(out,
                           splitDohLines(profileDoh),
                           profile->bean->serverResolverAllowLocalFallback,
                           "mode",
                           "doh_nameservers",
                           "fallback");
    }

    void emitAnyTLS(YAML::Emitter &out,
                    const std::shared_ptr<NekoGui::ProxyEntity> &profile,
                    const QString &groupKey,
                    const std::shared_ptr<NekoGui::Group> &group) {
        const auto bean = profile->AnyTLSBean();
        out << YAML::BeginMap;
        emitBaseProxyFields(out, profile, "anytls", groupKey);
        emitStringField(out, "password", bean->password, false);
        emitStringField(out, "sni", bean->sni);
        emitBoolField(out, "disable-sni", bean->disableSni);
        emitBoolField(out, "skip-cert-verify", bean->allowInsecure);
        emitStringListField(out, "alpn", splitCommaValues(bean->alpn));
        emitStringField(out, "client-fingerprint", bean->utlsFingerprint);
        emitStringField(out, "ca-str", bean->certificate);
        if (!bean->realityPublicKey.trimmed().isEmpty() || !bean->realityShortId.trimmed().isEmpty()) {
            out << YAML::Key << "reality-opts" << YAML::Value << YAML::BeginMap;
            emitStringField(out, "public-key", bean->realityPublicKey);
            emitStringField(out, "short-id", bean->realityShortId);
            out << YAML::EndMap;
        }
        emitStringField(out, "idle-session-check-interval", bean->idleSessionCheckInterval);
        emitStringField(out, "idle-session-timeout", bean->idleSessionTimeout);
        if (bean->minIdleSession > 0) emitIntField(out, "min-idle-session", bean->minIdleSession);
        const auto effectiveClient = profileEffectiveAnyTLSClient(bean, group);
        if (effectiveClient.isRuntimeValue()) emitStringField(out, "client", effectiveClient.value, false);
        const auto explicitClient = profileExplicitAnyTLSClient(bean);
        if (explicitClient.isRuntimeValue() && !sameClientSetting(explicitClient, groupClientDefault(group))) {
            out << YAML::Key << "x-client-override" << YAML::Value << YAML::BeginMap;
            emitStringField(out, "mode", explicitClient.mode, false);
            emitStringField(out, "value", explicitClient.value, false);
            out << YAML::EndMap;
        }
        emitServerResolverOverride(out, profile, group);
        out << YAML::EndMap;
    }

    void emitStreamBackedProxy(YAML::Emitter &out,
                               const std::shared_ptr<NekoGui::ProxyEntity> &profile,
                               const QString &groupKey,
                               const std::shared_ptr<NekoGui::Group> &group) {
        out << YAML::BeginMap;

        if (profile->type == "trojan" || profile->type == "vless") {
            const auto bean = profile->TrojanVLESSBean();
            emitBaseProxyFields(out, profile, profile->type, groupKey);
            if (profile->type == "vless") {
                emitStringField(out, "uuid", bean->password, false);
                emitStringField(out, "flow", bean->flow);
            } else {
                emitStringField(out, "password", bean->password, false);
            }
            emitTransport(out, bean->stream.get());
        } else if (profile->type == "vmess") {
            const auto bean = profile->VMessBean();
            emitBaseProxyFields(out, profile, "vmess", groupKey);
            emitStringField(out, "uuid", bean->uuid, false);
            emitIntField(out, "alterId", bean->aid);
            emitStringField(out, "cipher", bean->security, false);
            emitTransport(out, bean->stream.get());
        } else if (profile->type == "shadowsocks") {
            const auto bean = profile->ShadowSocksBean();
            emitBaseProxyFields(out, profile, "ss", groupKey);
            emitStringField(out, "cipher", bean->method, false);
            emitStringField(out, "password", bean->password, false);
            if (!bean->plugin.trimmed().isEmpty()) emitStringField(out, "plugin", bean->plugin);
            emitTransport(out, bean->stream.get());
        } else if (profile->type == "socks" || profile->type == "http") {
            const auto bean = profile->SocksHTTPBean();
            emitBaseProxyFields(out, profile, profile->type, groupKey);
            emitStringField(out, "username", bean->username);
            emitStringField(out, "password", bean->password);
            emitTransport(out, bean->stream.get());
        } else {
            emitBaseProxyFields(out, profile, profile->type, groupKey);
        }

        emitServerResolverOverride(out, profile, group);
        out << YAML::EndMap;
    }

    QString BuildMultiMapperExportYaml(const QList<std::shared_ptr<NekoGui::ProxyEntity>> &profiles) {
        QMap<int, QString> keysByGroup;
        QMap<int, std::shared_ptr<NekoGui::Group>> groupsById;
        QSet<QString> usedKeys;
        QList<int> groupOrder;
        QStringList rootDohs;

        for (const auto &profile: profiles) {
            if (profile == nullptr || profile->bean == nullptr) continue;
            auto group = NekoGui::profileManager->GetGroup(profile->gid);
            if (group == nullptr) continue;
            const auto key = ensureGroupKey(group, &keysByGroup, &usedKeys);
            if (!groupsById.contains(group->id)) {
                groupsById[group->id] = group;
                groupOrder << group->id;
            }
            appendUnique(&rootDohs, splitDohLines(group->default_server_resolver_doh));
            Q_UNUSED(key);
        }

        YAML::Emitter out;
        out.SetIndent(2);
        out << YAML::BeginMap;
        out << YAML::Key << "proxies" << YAML::Value << YAML::BeginSeq;
        for (const auto &profile: profiles) {
            if (profile == nullptr || profile->bean == nullptr) continue;
            const auto group = NekoGui::profileManager->GetGroup(profile->gid);
            if (group == nullptr) continue;
            const auto groupKey = keysByGroup.value(group->id);
            if (profile->type == "anytls") {
                emitAnyTLS(out, profile, groupKey, group);
            } else {
                emitStreamBackedProxy(out, profile, groupKey, group);
            }
        }
        out << YAML::EndSeq;

        if (!rootDohs.isEmpty()) {
            out << YAML::Key << "dns" << YAML::Value << YAML::BeginMap;
            out << YAML::Key << "proxy-server-nameserver" << YAML::Value;
            emitDohList(out, rootDohs);
            out << YAML::EndMap;
        }

        out << YAML::Key << "x-nekoray" << YAML::Value << YAML::BeginMap;
        emitStringField(out, "format", "compact-clash-yaml", false);
        emitIntField(out, "version", 1);
        emitStringField(out, "exported_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate), false);
        out << YAML::Key << "groups" << YAML::Value << YAML::BeginMap;
        for (const auto groupId: groupOrder) {
            const auto group = groupsById.value(groupId);
            out << YAML::Key << YAML::DoubleQuoted << utf8(keysByGroup.value(groupId)) << YAML::Value << YAML::BeginMap;
            emitStringField(out, "source_type", effectiveSourceType(group), false);
            emitStringField(out, "source", group->name, false);
            out << YAML::Key << "defaults" << YAML::Value << YAML::BeginMap;
            const auto client = groupClientDefault(group);
            if (client.isRuntimeValue()) {
                out << YAML::Key << "client" << YAML::Value << YAML::BeginMap;
                emitStringField(out, "mode", client.mode, false);
                emitStringField(out, "value", client.value, false);
                out << YAML::EndMap;
            }
            const auto dohs = splitDohLines(group->default_server_resolver_doh);
            if (!dohs.isEmpty()) {
                out << YAML::Key << "server_resolver" << YAML::Value;
                emitResolverObject(out,
                                   dohs,
                                   group->default_server_resolver_allow_local_fallback,
                                   "mode",
                                   "doh_nameservers",
                                   "fallback");
            }
            out << YAML::EndMap;
            if (!dohs.isEmpty()) {
                out << YAML::Key << "doh_nameservers" << YAML::Value;
                emitDohList(out, dohs);
            }
            emitSubscriptionInfo(out, parseSubscriptionInfo(group->info));
            out << YAML::EndMap;
        }
        out << YAML::EndMap;
        out << YAML::EndMap;
        out << YAML::EndMap;

        return QString::fromUtf8(out.c_str()) + "\n";
    }
#endif
} // namespace

QString BuildMultiMapperExportJson(const QList<std::shared_ptr<NekoGui::ProxyEntity>> &profiles) {
    QJsonObject groups;
    QMap<int, QJsonArray> itemsByGroup;
    QMap<int, QString> keysByGroup;
    QSet<QString> usedKeys;

    for (const auto &profile: profiles) {
        if (profile == nullptr || profile->bean == nullptr) continue;
        auto group = NekoGui::profileManager->GetGroup(profile->gid);
        if (group == nullptr) continue;
        auto uniqueKey = ensureGroupKey(group, &keysByGroup, &usedKeys);
        if (!groups.contains(uniqueKey)) {
            groups[uniqueKey] = QJsonObject{
                {"source_type", effectiveSourceType(group)},
                {"source", group->name},
                {"defaults", groupDefaults(group)},
                {"doh_nameservers", QList2QJsonArray(splitDohLines(group->default_server_resolver_doh))},
            };
            const auto subInfo = parseSubscriptionInfo(group->info);
            if (!subInfo.isEmpty()) {
                auto groupObj = groups[uniqueKey].toObject();
                groupObj["subscription_info"] = subInfo;
                groups[uniqueKey] = groupObj;
            }
        }
        itemsByGroup[group->id].append(buildItem(profile, group));
    }

    for (auto it = itemsByGroup.constBegin(); it != itemsByGroup.constEnd(); ++it) {
        const auto key = keysByGroup.value(it.key());
        auto groupObj = groups[key].toObject();
        groupObj["items"] = it.value();
        groups[key] = groupObj;
    }

    const QJsonObject root{
        {"format", "nekoray-multimapper-export"},
        {"version", 1},
        {"exported_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"groups", groups},
    };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString BuildMultiMapperExport(const QList<std::shared_ptr<NekoGui::ProxyEntity>> &profiles) {
#ifndef NKR_NO_YAML
    return BuildMultiMapperExportYaml(profiles);
#else
    return BuildMultiMapperExportJson(profiles);
#endif
}
} // namespace NekoGui_sub
