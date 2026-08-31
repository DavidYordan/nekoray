#include "AuxiliaryRouteCompiler.hpp"

#include <QJsonObject>

namespace NekoGui {
    namespace {
        bool IsExplicitRejectRule(const QJsonObject &rule) {
            return !rule.contains("outbound") &&
                   rule["action"].toString().trimmed().compare(
                       QStringLiteral("reject"),
                       Qt::CaseInsensitive) == 0;
        }

        QJsonObject ScopeRejectRuleToInbound(
            const QJsonObject &rule,
            const QString &inboundTag) {
            auto condition = rule;
            condition.remove("action");
            condition.remove("method");
            condition.remove("no_drop");

            QJsonObject scopedRule;
            if (condition.isEmpty()) {
                scopedRule = QJsonObject{
                    {"inbound", QJsonArray{inboundTag}},
                };
            } else {
                scopedRule = QJsonObject{
                    {"type", "logical"},
                    {"mode", "and"},
                    {"rules", QJsonArray{
                        QJsonObject{{"inbound", QJsonArray{inboundTag}}},
                        condition,
                    }},
                };
            }
            scopedRule["action"] = QStringLiteral("reject");
            if (rule.contains("method")) scopedRule["method"] = rule["method"];
            if (rule.contains("no_drop")) scopedRule["no_drop"] = rule["no_drop"];
            return scopedRule;
        }
    }

    QJsonArray BuildAuxiliaryRejectAndTerminalRules(
        const QString &inboundTag,
        const QString &outboundTag,
        const QJsonArray &ordinaryRules) {
        QJsonArray compiled;
        for (const auto &value: ordinaryRules) {
            if (!value.isObject()) continue;
            const auto rule = value.toObject();
            if (IsExplicitRejectRule(rule)) {
                compiled += ScopeRejectRuleToInbound(rule, inboundTag);
            }
        }
        compiled += QJsonObject{
            {"inbound", QJsonArray{inboundTag}},
            {"outbound", outboundTag},
        };
        return compiled;
    }
}
