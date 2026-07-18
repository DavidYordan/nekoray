#pragma once

#include <QJsonObject>
#include <QJsonArray>

#include "main/NekoGui.hpp"

namespace NekoGui_fmt {
    struct CoreObjOutboundBuildResult {
    public:
        QJsonObject outbound;
        QString error;
    };

    class AbstractBean : public JsonStore {
    public:
        int version;

        QString name = "";
        QString serverAddress = "127.0.0.1";
        int serverPort = 1080;

        QString custom_config = "";
        QString custom_outbound = "";

        QString serverResolverDohUpstreams = "";
        bool serverResolverAllowLocalFallback = true;
        bool inheritSubscriptionClient = true;
        bool inheritSubscriptionResolver = true;

        explicit AbstractBean(int version);

        //

        QString ToNekorayShareLink(const QString &type);

        [[nodiscard]] virtual QString DisplayAddress();

        [[nodiscard]] virtual QString DisplayName();

        virtual QString DisplayCoreType() { return software_core_name; };

        virtual QString DisplayType() { return {}; };

        virtual QString DisplayTypeAndName();

        //

        virtual CoreObjOutboundBuildResult BuildCoreObjSingBox() { return {}; };

        virtual QString ToShareLink() { return {}; };
    };

} // namespace NekoGui_fmt
