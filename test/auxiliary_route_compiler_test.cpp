#include "db/AuxiliaryRouteCompiler.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

namespace {
    bool Expect(bool condition, const char *message) {
        if (!condition) std::fprintf(stderr, "auxiliary_route_compiler_test: %s\n", message);
        return condition;
    }

    bool IsExactInbound(const QJsonObject &rule, const QString &tag) {
        const auto inbound = rule["inbound"].toArray();
        return inbound.size() == 1 && inbound[0].toString() == tag;
    }
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    using NekoGui::BuildAuxiliaryRejectAndTerminalRules;
    bool ok = true;

    const auto ordinaryRules = QJsonArray{
        QJsonObject{
            {"domain_suffix", QJsonArray{"blocked.example"}},
            {"action", "reject"},
        },
        QJsonObject{
            {"domain_suffix", QJsonArray{"must-use-normal-routing.example"}},
            {"outbound", "bypass"},
        },
        QJsonObject{
            {"inbound", QJsonArray{"mixed-in"}},
            {"action", "resolve"},
            {"strategy", "prefer_ipv4"},
        },
        QJsonObject{
            {"type", "logical"},
            {"mode", "or"},
            {"rules", QJsonArray{
                QJsonObject{{"ip_is_private", true}},
                QJsonObject{
                    {"domain_keyword", QJsonArray{"deny"}},
                    {"invert", true},
                },
            }},
            {"action", "reject"},
            {"method", "reply"},
            {"no_drop", true},
        },
        QJsonObject{
            {"outbound", "block"},
        },
        QJsonObject{
            {"action", "reject"},
        },
    };

    const auto compiled = BuildAuxiliaryRejectAndTerminalRules(
        QStringLiteral("aux-mixed-42"),
        QStringLiteral("aux-chain-42"),
        ordinaryRules);
    ok &= Expect(compiled.size() == 4,
                 "three explicit reject rules and one terminal binding must be emitted");

    if (compiled.size() == 4) {
        const auto domainReject = compiled[0].toObject();
        ok &= Expect(domainReject["type"].toString() == QStringLiteral("logical") &&
                         domainReject["mode"].toString() == QStringLiteral("and") &&
                         domainReject["action"].toString() == QStringLiteral("reject"),
                     "a conditional reject must be wrapped as an inbound-scoped logical rule");
        const auto domainConditions = domainReject["rules"].toArray();
        ok &= Expect(domainConditions.size() == 2 &&
                         IsExactInbound(domainConditions[0].toObject(), QStringLiteral("aux-mixed-42")) &&
                         domainConditions[1].toObject()["domain_suffix"].toArray() ==
                             QJsonArray{QStringLiteral("blocked.example")},
                     "the scoped reject must preserve its original domain condition");

        const auto logicalReject = compiled[1].toObject();
        const auto logicalConditions = logicalReject["rules"].toArray();
        ok &= Expect(logicalReject["action"].toString() == QStringLiteral("reject") &&
                         logicalReject["method"].toString() == QStringLiteral("reply") &&
                         logicalReject["no_drop"].toBool() &&
                         logicalConditions.size() == 2 &&
                         logicalConditions[1].toObject()["type"].toString() == QStringLiteral("logical") &&
                         logicalConditions[1].toObject()["mode"].toString() == QStringLiteral("or"),
                     "logical reject conditions and reject action options must survive scoping");

        const auto unconditionalReject = compiled[2].toObject();
        ok &= Expect(IsExactInbound(unconditionalReject, QStringLiteral("aux-mixed-42")) &&
                         unconditionalReject["action"].toString() == QStringLiteral("reject") &&
                         !unconditionalReject.contains("outbound"),
                     "an unconditional reject must become exact to the auxiliary inbound");

        const auto terminal = compiled[3].toObject();
        ok &= Expect(IsExactInbound(terminal, QStringLiteral("aux-mixed-42")) &&
                         terminal["outbound"].toString() == QStringLiteral("aux-chain-42") &&
                         terminal.keys().size() == 2,
                     "the final rule must remain the exact auxiliary line binding");
    }

    for (int index = 0; index + 1 < compiled.size(); ++index) {
        const auto rule = compiled[index].toObject();
        ok &= Expect(!rule.contains("outbound"),
                     "no pre-terminal auxiliary rule may redirect to direct, bypass, or another line");
    }

    return ok ? 0 : 1;
}
