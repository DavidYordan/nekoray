#pragma once

#include <QString>
#include <QList>
#include <QMutex>

#include <atomic>

#include "TrafficData.hpp"

namespace NekoGui_traffic {
    class TrafficLooper {
    public:
        std::atomic_bool loop_enabled = false;
        std::atomic_bool looping = false;
        QMutex loop_mutex;

        QList<TrafficBinding> items;
        std::shared_ptr<TrafficData> proxy;

        void UpdateAll();

        void Loop();

    private:
        TrafficData *bypass = new TrafficData("bypass");

        [[nodiscard]] static TrafficData *update_stats(TrafficData* data, const std::string& outboundTag);

        [[nodiscard]] static QJsonArray get_connection_list();
    };

    extern TrafficLooper *trafficLooper;
} // namespace NekoGui_traffic
