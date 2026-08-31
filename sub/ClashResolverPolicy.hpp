#pragma once

#include <QString>
#include <QStringList>

#ifndef NKR_NO_YAML
#include <yaml-cpp/yaml.h>

namespace NekoGui_sub {
    enum class ClashResolverSource {
        None,
        ProxyServerNameserver,
        Nameserver,
    };

    struct ClashResolverSelection {
        ClashResolverSource source = ClashResolverSource::None;
        QStringList dohUpstreams;
        QStringList invalidDohEntries;
        QStringList unsupportedEntries;
    };

    ClashResolverSelection ExtractClashServerResolver(const YAML::Node& root);

    QString ClashResolverSourceName(ClashResolverSource source);
} // namespace NekoGui_sub
#endif
