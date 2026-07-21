#pragma once

#include "ProxyEntity.hpp"

#include <QMap>

namespace NekoGui {
    struct ResolverBindingRequest {
        int outboundIndex = -1;
        QString outboundTag;
        QString server;
        QStringList dohUpstreams;
    };

    struct ManagedMixedBinding {
        QString inboundTag;
        int listenPort = -1;
        QString outboundTag;
        bool allowResolveBeforeTerminal = false;
        QJsonObject expectedInbound;
        // Snapshot after all profile-level/custom_outbound generation, but
        // before top-level custom_config is merged.  Every reachable outbound
        // (including its detour) must be identical so one managed port cannot
        // be redirected or silently rewritten into a different managed line.
        QMap<QString, QJsonObject> expectedOutbounds;
    };

    class BuildConfigResult {
    public:
        QString error;
        QJsonObject coreConfig;
        // Immutable property of the final configuration that passed the
        // managed-TUN validator. Runtime state must commit this value instead
        // of re-reading mutable UI settings after the Start RPC.
        bool managedInternalTun = false;

        QList<NekoGui_traffic::TrafficBinding> outboundStats; // all, but not including "bypass" "block"
        std::shared_ptr<NekoGui_traffic::TrafficData> outboundStat;         // main
        QStringList ignoreConnTag;
    };

    class BuildConfigStatus {
    public:
        std::shared_ptr<BuildConfigResult> result;
        std::shared_ptr<ProxyEntity> ent;
        bool forTest;
        bool forExport;

        // priv
        QList<int> globalProfiles;

        // xxList uses the legacy route-rule string list format.

        QStringList domainListDNSRemote;
        QStringList domainListDNSDirect;
        QStringList domainListRemote;
        QStringList domainListDirect;
        QStringList ipListRemote;
        QStringList ipListDirect;
        QStringList domainListBlock;
        QStringList ipListBlock;

        // config format

        QJsonArray routingRules;
        QJsonArray frontRoutingRules;
        QJsonArray inbounds;
        QJsonArray outbounds;
        QList<ResolverBindingRequest> resolverBindingRequests;
        QList<ManagedMixedBinding> managedMixedBindings;
    };

    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport);

    void BuildConfigSingBox(const std::shared_ptr<BuildConfigStatus> &status);

    QString BuildChain(int chainId, const std::shared_ptr<BuildConfigStatus> &status);

    QString BuildChainInternal(int chainId, const QList<std::shared_ptr<ProxyEntity>> &ents,
                               const std::shared_ptr<BuildConfigStatus> &status);

    QString WriteVPNSingBoxConfig();

    QString WriteVPNLinuxScript(const QString &configPath);
} // namespace NekoGui
