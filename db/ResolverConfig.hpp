#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace NekoGui_resolver {
    inline constexpr int SubscriptionResolverPolicyVersion = 1;

    QJsonObject BuildDomainResolverObject(const QString& server, const QString& strategy = {});

    QString StableTag(const QString& prefix, const QString& key);

    QStringList ParseDohUpstreams(const QString& raw);

    bool ValidateDohUpstream(const QString& raw, QString* error = nullptr);

    QJsonObject BuildProviderDohServer(
        const QString& tag,
        const QString& rawUrl,
        const QString& nativeBootstrapResolverTag,
        QString* error = nullptr);

    QJsonObject BuildProviderResolverGroup(const QString& tag, const QStringList& primaryTags);
} // namespace NekoGui_resolver
