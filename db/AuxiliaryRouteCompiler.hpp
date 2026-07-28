#pragma once

#include <QJsonArray>
#include <QString>

namespace NekoGui {
    // ordinaryRules must already use sing-box action form (for example,
    // outbound "block" normalized to action "reject").
    QJsonArray BuildAuxiliaryRejectAndTerminalRules(
        const QString &inboundTag,
        const QString &outboundTag,
        const QJsonArray &ordinaryRules);
}
