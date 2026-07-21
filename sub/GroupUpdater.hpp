#pragma once

#include "db/Database.hpp"

namespace NekoGui_sub {
    class RawUpdater {
    public:
        void updateClash(const QString &str);

        void update(const QString &str);

        int gid_add_to = -1; // Import into the selected group when negative.

        // Parsing only stages entities here. GroupUpdater commits them after
        // the complete input has parsed successfully and yielded at least one
        // supported profile.
        QList<std::shared_ptr<NekoGui::ProxyEntity>> updated_order;

        bool parse_failed = false;
        QString parse_error;

        QString detected_source_type;
        QStringList detected_doh_upstreams;
        QStringList detected_invalid_doh_upstreams;
    };

    class GroupUpdater : public QObject {
        Q_OBJECT

    public:
        void AsyncUpdate(const QString &str, int _sub_gid = -1, const std::function<void()> &finish = nullptr);

        int Update(const QString &_str, int _sub_gid = -1, bool _not_sub_as_url = false,
                   bool _create_new_group = false);

    signals:

        void asyncUpdateCallback(int gid);
    };

    extern GroupUpdater *groupUpdater;
} // namespace NekoGui_sub

// 更新所有订阅 关闭分组窗口时 更新动作继续执行
void UI_update_all_groups(bool onlyAllowed = false);
