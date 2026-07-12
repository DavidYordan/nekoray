#pragma once

#include "fmt/AbstractBean.hpp"

namespace NekoGui_fmt {
    class AnyTLSBean : public AbstractBean {
    public:
        QString password = "";

        QString idleSessionCheckInterval = "";
        QString idleSessionTimeout = "";
        int minIdleSession = 0;

        bool allowInsecure = false;
        bool disableSni = false;
        QString sni = "";
        QString alpn = "";
        QString certificate = "";
        QString utlsFingerprint = "";
        QString realityPublicKey = "";
        QString realityShortId = "";

        AnyTLSBean() : AbstractBean(0) {
            _add(new configItem("pass", &password, itemType::string));
            _add(new configItem("idle_chk", &idleSessionCheckInterval, itemType::string));
            _add(new configItem("idle_timeout", &idleSessionTimeout, itemType::string));
            _add(new configItem("min_idle", &minIdleSession, itemType::integer));
            _add(new configItem("insecure", &allowInsecure, itemType::boolean));
            _add(new configItem("disable_sni", &disableSni, itemType::boolean));
            _add(new configItem("sni", &sni, itemType::string));
            _add(new configItem("alpn", &alpn, itemType::string));
            _add(new configItem("cert", &certificate, itemType::string));
            _add(new configItem("utls", &utlsFingerprint, itemType::string));
            _add(new configItem("pbk", &realityPublicKey, itemType::string));
            _add(new configItem("sid", &realityShortId, itemType::string));
        };

        QString DisplayType() override { return "AnyTLS"; };

        CoreObjOutboundBuildResult BuildCoreObjSingBox() override;

        bool TryParseLink(const QString &link);

        QString ToShareLink() override;
    };
} // namespace NekoGui_fmt
