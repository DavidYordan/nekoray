#pragma once

#include "main/NekoGui.hpp"
#include "ProxyEntity.hpp"

namespace NekoGui {
    class Group : public JsonStore {
    public:
        int id = -1;
        bool archive = false;
        bool skip_auto_update = false;
        QString name = "";
        QString url = "";
        QString info = "";
        qint64 sub_last_update = 0;
        int front_proxy_id = -1;

        QString source_type = "";
        QString default_client_mode = "";
        QString default_client_value = "";
        QString default_client_source = "";
        QString default_server_resolver_doh = "";
        QString default_server_resolver_source = "";
        QString default_server_resolver_origin = "";
        int default_server_resolver_policy_version = 0;
        bool default_server_resolver_allow_local_fallback = false;

        // list ui
        bool manually_column_width = false;
        QList<int> column_width;
        QList<int> order;

        Group();

        [[nodiscard]] bool DefaultClientManagedBySubscription() const;

        [[nodiscard]] bool DefaultResolverManagedBySubscription() const;

        void SetDefaultClientManagedBySubscription(bool enabled);

        void SetDefaultResolverManagedBySubscription(bool enabled);

        // 按 id 顺序
        [[nodiscard]] QList<std::shared_ptr<ProxyEntity>> Profiles() const;

        // 按 显示 顺序
        [[nodiscard]] QList<std::shared_ptr<ProxyEntity>> ProfilesWithOrder() const;
    };
} // namespace NekoGui
