#include "ClashResolverPolicy.hpp"

#ifndef NKR_NO_YAML

#include "db/ResolverConfig.hpp"

#include <QSet>
#include <QUrl>

namespace NekoGui_sub {
    namespace {
        YAML::Node FirstDefinedChild(const YAML::Node& node, const std::initializer_list<const char*>& keys) {
            for (const auto* key: keys) {
                const auto child = node[key];
                if (child.IsDefined()) return child;
            }
            return YAML::Node(YAML::NodeType::Undefined);
        }

        void ClassifyValue(const YAML::Node& node, ClashResolverSelection& result, QSet<QString>& seen) {
            if (!node.IsScalar()) {
                result.invalidDohEntries += QStringLiteral("<non-scalar resolver item>");
                return;
            }
            try {
                const auto value = QString::fromStdString(node.as<std::string>()).trimmed();
                if (value.isEmpty() || seen.contains(value)) return;
                seen.insert(value);

                const auto url = QUrl(value);
                if (url.scheme().compare("https", Qt::CaseInsensitive) != 0) {
                    result.unsupportedEntries += value;
                    return;
                }
                QString error;
                if (NekoGui_resolver::ValidateDohUpstream(value, &error)) {
                    result.dohUpstreams += value;
                } else {
                    result.invalidDohEntries += value;
                }
            } catch (const YAML::Exception&) {
                result.invalidDohEntries += QStringLiteral("<invalid resolver scalar>");
            }
        }

        void ClassifyEntries(const YAML::Node& node, ClashResolverSelection& result) {
            QSet<QString> seen;
            if (node.IsSequence()) {
                for (const auto& item: node) ClassifyValue(item, result, seen);
            } else if (node.IsScalar()) {
                ClassifyValue(node, result, seen);
            } else if (!node.IsNull()) {
                result.invalidDohEntries += QStringLiteral("<invalid resolver field>");
            }
        }
    } // namespace

    ClashResolverSelection ExtractClashServerResolver(const YAML::Node& root) {
        ClashResolverSelection result;
        const auto dns = root["dns"];
        if (!dns.IsMap()) return result;

        const auto explicitProxyResolver = FirstDefinedChild(
            dns,
            {"proxy-server-nameserver", "proxy_server_nameserver"});
        if (explicitProxyResolver.IsDefined()) {
            result.source = ClashResolverSource::ProxyServerNameserver;
            ClassifyEntries(explicitProxyResolver, result);
            // Presence is authoritative. In particular, an unsupported local
            // UDP resolver must not make us borrow unrelated ordinary DoH
            // entries from dns.nameserver.
            return result;
        }

        const auto ordinaryNameserver = dns["nameserver"];
        if (ordinaryNameserver.IsDefined()) {
            result.source = ClashResolverSource::Nameserver;
            ClassifyEntries(ordinaryNameserver, result);
        }
        return result;
    }

    QString ClashResolverSourceName(ClashResolverSource source) {
        switch (source) {
            case ClashResolverSource::ProxyServerNameserver:
                return QStringLiteral("proxy-server-nameserver");
            case ClashResolverSource::Nameserver:
                return QStringLiteral("nameserver");
            case ClashResolverSource::None:
            default:
                return QStringLiteral("none");
        }
    }
} // namespace NekoGui_sub

#endif
