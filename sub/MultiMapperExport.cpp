#include "MultiMapperExport.hpp"

#include "fmt/includes.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

namespace NekoGui_sub {
namespace {
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
        if (!group->source_type.trimmed().isEmpty()) return group->source_type.trimmed();
        return group->url.trimmed().isEmpty() ? "manual" : "subscription";
    }

    QJsonObject groupDefaults(const std::shared_ptr<NekoGui::Group> &group) {
        QJsonObject defaults;
        QJsonObject client;
        auto mode = group == nullptr ? QString{} : group->default_client_mode.trimmed().toLower();
        auto value = group == nullptr ? QString{} : group->default_client_value.trimmed();
        if (mode.isEmpty() && effectiveSourceType(group) == "clash") {
            mode = "mihomo";
            value = "mihomo/1.19.28";
        }
        if (!mode.isEmpty() && mode != "native") {
            client["mode"] = mode;
            client["value"] = mode == "mihomo" && value.isEmpty() ? "mihomo/1.19.28" : value;
            defaults["client"] = client;
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
        if (!stream->alpn.isEmpty()) tlsSettings["alpn"] = QList2QJsonArray(stream->alpn.split(",", Qt::SkipEmptyParts));
        if (!stream->utlsFingerprint.isEmpty()) tlsSettings["fingerprint"] = stream->utlsFingerprint;
        if (!stream->reality_pbk.isEmpty()) tlsSettings["publicKey"] = stream->reality_pbk;
        if (!stream->reality_sid.isEmpty()) tlsSettings["shortId"] = stream->reality_sid;
        if (!stream->security.isEmpty()) streamObj[stream->security + "Settings"] = tlsSettings;
        return streamObj;
    }

    QJsonObject buildItem(const std::shared_ptr<NekoGui::ProxyEntity> &profile, const std::shared_ptr<NekoGui::Group> &group) {
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
            if (!bean->alpn.isEmpty()) tls["alpn"] = QList2QJsonArray(bean->alpn.split(",", Qt::SkipEmptyParts));
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
            const auto mode = bean->anytlsClientMode.trimmed().toLower();
            const auto groupMode = group == nullptr ? QString{} : group->default_client_mode.trimmed().toLower();
            const auto groupValue = group == nullptr ? QString{} : group->default_client_value.trimmed();
            const auto value = mode == "mihomo" ? "mihomo/1.19.28" : bean->anytlsClientValue.trimmed();
            const auto matchesGroupDefault =
                (mode == groupMode) &&
                (mode == "mihomo" || value == groupValue || (groupValue.isEmpty() && value.isEmpty()));
            if (!mode.isEmpty() && mode != "native" && !matchesGroupDefault) {
                item["client_override"] = QJsonObject{
                    {"mode", mode},
                    {"value", value},
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
} // namespace

QString BuildMultiMapperExportJson(const QList<std::shared_ptr<NekoGui::ProxyEntity>> &profiles) {
    QJsonObject groups;
    QMap<int, QJsonArray> itemsByGroup;
    QMap<int, QString> keysByGroup;

    for (const auto &profile: profiles) {
        if (profile == nullptr || profile->bean == nullptr) continue;
        auto group = NekoGui::profileManager->GetGroup(profile->gid);
        if (group == nullptr) continue;
        auto key = group->name.trimmed().isEmpty() ? QStringLiteral("group-%1").arg(group->id) : group->name.trimmed();
        if (!keysByGroup.contains(group->id)) {
            auto uniqueKey = key;
            int suffix = 2;
            while (groups.contains(uniqueKey)) uniqueKey = key + QStringLiteral(" (%1)").arg(suffix++);
            keysByGroup[group->id] = uniqueKey;
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
} // namespace NekoGui_sub
