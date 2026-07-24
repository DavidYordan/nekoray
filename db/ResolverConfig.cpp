#include "ResolverConfig.hpp"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace NekoGui_resolver {
    namespace {
        bool IsLiteralIp(const QString& value) {
            QHostAddress address;
            return address.setAddress(value);
        }
    } // namespace

    QJsonObject BuildDomainResolverObject(const QString& server, const QString& strategy) {
        QJsonObject resolver{{"server", server}};
        const auto normalizedStrategy = strategy.trimmed();
        if (!normalizedStrategy.isEmpty() && normalizedStrategy.compare("AsIs", Qt::CaseInsensitive) != 0) {
            resolver["strategy"] = normalizedStrategy;
        }
        return resolver;
    }

    QString StableTag(const QString& prefix, const QString& key) {
        const auto digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
        return prefix + "-" + digest;
    }

    QStringList ParseDohUpstreams(const QString& raw) {
        auto normalized = raw;
        normalized.replace(",", "\n");
        QStringList result;
        QSet<QString> seen;
        for (const auto& line: normalized.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts)) {
            const auto value = line.trimmed();
            if (value.isEmpty() || value.startsWith('#') || seen.contains(value)) continue;
            seen.insert(value);
            result += value;
        }
        return result;
    }

    bool ValidateDohUpstream(const QString& raw, QString* error) {
        if (error != nullptr) error->clear();
        const auto label = raw.trimmed();
        const auto url = QUrl(label);
        auto fail = [&](const QString& message) {
            if (error != nullptr) *error = QStringLiteral("%1: %2").arg(label, message);
            return false;
        };
        if (!url.isValid() || url.scheme().compare("https", Qt::CaseInsensitive) != 0) {
            return fail(QStringLiteral("DoH URL must use https scheme"));
        }
        if (url.host().isEmpty()) return fail(QStringLiteral("DoH URL must have host"));
        if (!url.userName().isEmpty() || !url.password().isEmpty()) {
            return fail(QStringLiteral("DoH URL must not include credentials"));
        }
        if (url.hasQuery()) return fail(QStringLiteral("DoH URL must not include query"));
        if (url.hasFragment()) return fail(QStringLiteral("DoH URL must not include fragment"));
        return true;
    }

    QJsonObject BuildProviderDohServer(
        const QString& tag,
        const QString& rawUrl,
        const QString& nativeBootstrapResolverTag,
        QString* error) {
        if (!ValidateDohUpstream(rawUrl, error)) return {};

        const auto url = QUrl(rawUrl.trimmed());
        const auto host = url.host();
        QJsonObject server{
            {"tag", tag},
            {"type", "https"},
            {"server", host},
            {"server_port", url.port(443)},
        };
        if (!url.path().isEmpty()) server["path"] = url.path();

        if (!IsLiteralIp(host)) {
            if (nativeBootstrapResolverTag.trimmed().isEmpty()) {
                if (error != nullptr) {
                    *error = QStringLiteral("DoH endpoint '%1' is a domain but the native NekoRay bootstrap resolver tag is empty.")
                                 .arg(host);
                }
                return {};
            }
            server["domain_resolver"] = BuildDomainResolverObject(nativeBootstrapResolverTag);
            server["tls"] = QJsonObject{
                {"enabled", true},
                {"server_name", host},
            };
        }
        return server;
    }

    QJsonObject BuildProviderResolverGroup(const QString& tag, const QStringList& primaryTags) {
        QJsonArray primary;
        for (const auto& primaryTag: primaryTags) primary += primaryTag;
        return QJsonObject{
            {"tag", tag},
            {"type", "routefluent_resolver_group"},
            {"primary", primary},
        };
    }
} // namespace NekoGui_resolver
