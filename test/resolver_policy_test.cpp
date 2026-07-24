#include "db/ResolverConfig.hpp"
#include "sub/ClashResolverPolicy.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>

#include <yaml-cpp/yaml.h>

#include <cstdio>

namespace {
    bool Expect(bool condition, const char* message) {
        if (!condition) std::fprintf(stderr, "resolver_policy_test: %s\n", message);
        return condition;
    }
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using namespace NekoGui_resolver;
    using namespace NekoGui_sub;
    bool ok = true;

    const auto wd = ExtractClashServerResolver(YAML::Load(R"(
dns:
  proxy-server-nameserver:
    - udp://127.0.0.1:53
  nameserver:
    - https://ordinary.example/dns-query
)"));
    ok &= Expect(wd.source == ClashResolverSource::ProxyServerNameserver,
                 "an explicit proxy-server-nameserver field must be authoritative");
    ok &= Expect(wd.dohUpstreams.isEmpty(),
                 "unsupported explicit proxy resolver entries must not borrow ordinary nameserver DoH");
    ok &= Expect(wd.unsupportedEntries.size() == 1,
                 "the unsupported explicit resolver must remain observable");

    const auto nex = ExtractClashServerResolver(YAML::Load(R"(
dns:
  nameserver:
    - https://resolver-a.example/dns-query/token
    - 223.5.5.5
    - https://resolver-b.example/dns-query/token
    - https://resolver-c.example/dns-query/token
)"));
    ok &= Expect(nex.source == ClashResolverSource::Nameserver,
                 "ordinary nameserver must be selected only when proxy-server-nameserver is absent");
    ok &= Expect(nex.dohUpstreams.size() == 3,
                 "all provider-carried HTTPS nameservers must be retained in source order");
    ok &= Expect(nex.unsupportedEntries.size() == 1,
                 "non-DoH ordinary nameservers must be ignored without becoming a fallback");

    const auto explicitDoh = ExtractClashServerResolver(YAML::Load(R"(
dns:
  proxy_server_nameserver:
    - https://proxy-only.example/dns-query
  nameserver:
    - https://must-not-be-used.example/dns-query
)"));
    ok &= Expect(explicitDoh.dohUpstreams == QStringList{QStringLiteral("https://proxy-only.example/dns-query")},
                 "explicit proxy server DoH must override ordinary nameserver entries");

    const auto invalidDoh = ExtractClashServerResolver(YAML::Load(R"(
dns:
  nameserver:
    - https://resolver.example/dns-query#unsupported-modifier
)"));
    ok &= Expect(invalidDoh.dohUpstreams.isEmpty() && invalidDoh.invalidDohEntries.size() == 1,
                 "malformed or unsupported HTTPS DoH syntax must fail validation explicitly");

    const auto malformedResolver = ExtractClashServerResolver(YAML::Load(R"(
dns:
  nameserver:
    - https://resolver.example/dns-query
    - server: https://nested.example/dns-query
)"));
    ok &= Expect(malformedResolver.dohUpstreams.size() == 1 &&
                     malformedResolver.invalidDohEntries.size() == 1,
                 "non-scalar resolver entries must fail explicitly without discarding valid siblings");

    QString error;
    const auto domainDoh = BuildProviderDohServer(
        QStringLiteral("provider-doh"),
        QStringLiteral("https://resolver.example:4443/dns-query/token"),
        QStringLiteral("dns-local"),
        &error);
    ok &= Expect(error.isEmpty() && !domainDoh.isEmpty(),
                 "a domain DoH endpoint must be buildable with native bootstrap");
    ok &= Expect(domainDoh["domain_resolver"].toObject() == QJsonObject{{"server", "dns-local"}},
                 "DoH endpoint bootstrap must use the native resolver without forcing an IP strategy");
    ok &= Expect(domainDoh["tls"].toObject()["server_name"].toString() == QStringLiteral("resolver.example"),
                 "DoH TLS SNI must preserve the subscription endpoint hostname");

    const auto ipDoh = BuildProviderDohServer(
        QStringLiteral("provider-doh-ip"),
        QStringLiteral("https://1.1.1.1/dns-query"),
        QStringLiteral("dns-local"),
        &error);
    ok &= Expect(!ipDoh.contains("domain_resolver") && !ipDoh.contains("tls"),
                 "literal-IP DoH endpoints must not perform bootstrap DNS");

    const auto defaultPathDoh = BuildProviderDohServer(
        QStringLiteral("provider-default-path"),
        QStringLiteral("https://resolver.example"),
        QStringLiteral("dns-local"),
        &error);
    ok &= Expect(!defaultPathDoh.contains("path"),
                 "an omitted DoH path must retain sing-box's native /dns-query default");

    const auto group = BuildProviderResolverGroup(
        QStringLiteral("provider-group"),
        {QStringLiteral("provider-doh"), QStringLiteral("provider-doh-2")});
    ok &= Expect(group["primary"].toArray().size() == 2 &&
                     !group.contains("fallback") &&
                     !group.contains("fallback_enabled") &&
                     !group.contains("local_only"),
                 "provider groups may fail over only among provider DoH primaries, never to local DNS");

    ok &= Expect(
        ParseDohUpstreams(QStringLiteral("https://a.example/dns-query, https://a.example/dns-query\nhttps://b.example/dns-query"))
            .size() == 2,
        "persisted DoH lists must preserve order and remove duplicates");

    return ok ? 0 : 1;
}
