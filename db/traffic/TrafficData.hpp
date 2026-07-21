#pragma once

#include "main/NekoGui.hpp"

namespace NekoGui_traffic {
    class TrafficData : public JsonStore {
    public:
        int id = -1; // ent id
        std::string tag;

        long long downlink = 0;
        long long uplink = 0;
        long long downlink_rate = 0;
        long long uplink_rate = 0;

        long long last_update = 0;

        explicit TrafficData(std::string tag) {
            this->tag = std::move(tag);
            _add(new configItem("dl", &downlink, itemType::integer64));
            _add(new configItem("ul", &uplink, itemType::integer64));
        };

        void Reset() {
            downlink = 0;
            uplink = 0;
            downlink_rate = 0;
            uplink_rate = 0;
        }

        [[nodiscard]] QString DisplaySpeed() const {
            return UNICODE_LRO + QStringLiteral("%1↑ %2↓").arg(ReadableSize(uplink_rate), ReadableSize(downlink_rate));
        }

        [[nodiscard]] QString DisplayTraffic() const {
            if (downlink + uplink == 0) return "";
            return UNICODE_LRO + QStringLiteral("%1↑ %2↓").arg(ReadableSize(uplink), ReadableSize(downlink));
        }
    };

    // Immutable routing identity produced by ConfigBuilder. Traffic counters
    // remain attached to the profile-owned TrafficData object, while each
    // generation keeps its own outbound tag and profile id by value. Building
    // a test/export/candidate must never rewrite live runtime telemetry tags.
    struct TrafficBinding {
        int profileId = -1;
        std::string outboundTag;
        std::shared_ptr<TrafficData> data;
    };
} // namespace NekoGui_traffic
