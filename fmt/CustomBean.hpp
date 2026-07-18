#pragma once

#include "fmt/AbstractBean.hpp"

namespace NekoGui_fmt {
    class CustomBean : public AbstractBean {
    public:
        QString core;
        QString config_simple;

        CustomBean() : AbstractBean(0) {
            _add(new configItem("core", &core, itemType::string));
            _add(new configItem("cs", &config_simple, itemType::string));
        };

        QString DisplayType() override {
            if (core == "internal") {
                auto obj = QString2QJsonObject(config_simple);
                return obj["type"].toString();
            } else if (core == "internal-full") {
                return software_core_name + " config";
            }
            return core;
        };

        QString DisplayCoreType() override { return software_core_name; };

        QString DisplayAddress() override {
            if (core == "internal") {
                auto obj = QString2QJsonObject(config_simple);
                return ::DisplayAddress(obj["server"].toString(), obj["server_port"].toInt());
            } else if (core == "internal-full") {
                return {};
            }
            return AbstractBean::DisplayAddress();
        };

        CoreObjOutboundBuildResult BuildCoreObjSingBox() override;
    };
} // namespace NekoGui_fmt
