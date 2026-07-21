#include "Database.hpp"

#include "fmt/includes.h"
#include "main/ConfigRecovery.hpp"
#include "main/ConfigTransaction.hpp"

#include <QFile>
#include <QDir>
#include <QColor>
#include <QFileInfo>

#include <limits>

namespace NekoGui {

    namespace {
        void recordRecoveryIssue(
            const QString &path,
            const QByteArray &content,
            const QString &reason) {
            const auto quarantine = NekoGui_ConfigRecovery::RecordQuarantine(path, content, reason);
            if (!quarantine.succeeded) {
                qCritical() << "Could not record config recovery issue:" << path << quarantine.error;
            } else {
                qWarning() << "Config recovery issue recorded:" << path << reason << quarantine.snapshotPath;
            }
        }

        bool serializeStoreForTransaction(JsonStore *store, QByteArray *content, QString *error) {
            store->last_save_succeeded = false;
            if (store->callback_before_save != nullptr) {
                const auto validationError = store->callback_before_save();
                if (!validationError.isEmpty()) {
                    *error = QStringLiteral("Refusing to stage invalid config %1: %2")
                                 .arg(store->fn, validationError);
                    return false;
                }
            }
            if (store->save_control_no_save) {
                *error = QStringLiteral("Config store is not writable in this mode: %1").arg(store->fn);
                return false;
            }
            if (store->load_failed_existing) {
                *error = QStringLiteral("Refusing to stage a config that failed to load: %1").arg(store->fn);
                return false;
            }

            *content = store->ToJsonBytes();
            return true;
        }

        NekoGui_ConfigTransaction::FileState loadedStoreState(const JsonStore *store) {
            return {QFileInfo::exists(store->fn), store->last_save_content};
        }
    }

    ProfileManager *profileManager = new ProfileManager();

    ProfileManager::ProfileManager() : JsonStore("groups/pm.json") {
        _add(new configItem("groups", &groupsTabOrder, itemType::integerList));
    }

    QList<int> filterIntJsonFile(const QString &path) {
        QList<int> result;
        QDir dr(path);
        auto entryList = dr.entryList(QDir::Files);
        for (auto e: entryList) {
            e = e.toLower();
            if (!e.endsWith(".json", Qt::CaseInsensitive)) continue;
            e = e.remove(".json", Qt::CaseInsensitive);
            bool ok;
            auto id = e.toInt(&ok);
            if (ok) {
                result << id;
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    void ProfileManager::LoadManager() {
        JsonStore::Load();
        //
        profiles = {};
        groups = {};
        profilesIdOrder = filterIntJsonFile("profiles");
        groupsIdOrder = filterIntJsonFile("groups");
        // Load Proxys
        for (auto id: profilesIdOrder) {
            auto ent = LoadProxyEntity(QStringLiteral("profiles/%1.json").arg(id));
            // Preserve unreadable and unknown profiles on disk. A later version
            // may understand them, and silently deleting user data is never a
            // valid compatibility strategy.
            if (ent == nullptr || ent->bean == nullptr || ent->bean->version == -114514) {
                qWarning() << "Profile was not loaded; original file is preserved:" << id;
                continue;
            }
            if (ent->id != id) {
                recordRecoveryIssue(
                    ent->fn,
                    ent->last_save_content,
                    QStringLiteral("Profile filename id %1 does not match stored id %2.").arg(id).arg(ent->id));
                qWarning() << "Profile was not loaded because its stored id does not match its filename:" << id;
                continue;
            }
            profiles[id] = ent;
        }
        // Load Groups
        auto loadedOrder = groupsTabOrder;
        groupsTabOrder = {};
        for (auto id: groupsIdOrder) {
            auto ent = LoadGroup(QStringLiteral("groups/%1.json").arg(id));
            if (ent == nullptr) {
                qWarning() << "Group was not loaded; original file is preserved:" << id;
                continue;
            }
            if (ent->id != id) {
                recordRecoveryIssue(
                    ent->fn,
                    ent->last_save_content,
                    QStringLiteral("Group filename id %1 does not match stored id %2.").arg(id).arg(ent->id));
                qWarning() << "Group was not loaded because its stored id does not match its filename:" << id;
                continue;
            }
            // Ensure order contains every group
            if (!loadedOrder.contains(id)) {
                loadedOrder << id;
            }
            groups[id] = ent;
        }
        // Ensure groups contains order
        for (auto id: loadedOrder) {
            if (groups.count(id)) {
                groupsTabOrder << id;
            }
        }
        for (auto id: loadedOrder) {
            if (!groups.count(id)) {
                recordRecoveryIssue(
                    fn,
                    last_save_content,
                    QStringLiteral("Group order references missing or unreadable group %1.").arg(id));
            }
        }
        for (const auto &[id, profile]: profiles) {
            if (!groups.count(profile->gid)) {
                recordRecoveryIssue(
                    profile->fn,
                    profile->last_save_content,
                    QStringLiteral("Profile %1 references missing or unreadable group %2.")
                        .arg(id)
                        .arg(profile->gid));
            }
        }
        for (const auto &[id, group]: groups) {
            QStringList missingOrderProfiles;
            for (const auto profileId: group->order) {
                if (!profiles.count(profileId)) missingOrderProfiles.append(QString::number(profileId));
            }
            if (!missingOrderProfiles.isEmpty()) {
                recordRecoveryIssue(
                    group->fn,
                    group->last_save_content,
                    QStringLiteral("Group %1 order references missing or unreadable profiles: %2.")
                        .arg(id)
                        .arg(missingOrderProfiles.join(", ")));
            }
            if (group->front_proxy_id >= 0 && !profiles.count(group->front_proxy_id)) {
                recordRecoveryIssue(
                    group->fn,
                    group->last_save_content,
                    QStringLiteral("Group %1 references missing front proxy profile %2.")
                        .arg(id)
                        .arg(group->front_proxy_id));
            }
        }
        if (!groups.empty() && !groups.count(dataStore->current_group)) {
            recordRecoveryIssue(
                dataStore->fn,
                dataStore->last_save_content,
                QStringLiteral("Current group references missing or unreadable group %1.")
                    .arg(dataStore->current_group));
        }
        // First setup
        if (groups.empty()) {
            auto defaultGroup = NekoGui::ProfileManager::NewGroup();
            defaultGroup->name = QObject::tr("Default");
            if (NekoGui::profileManager->AddGroup(defaultGroup)) {
                dataStore->current_group = defaultGroup->id;
            }
        }
        //
        if (dataStore->flag_reorder) {
            qCritical() << "Unsafe legacy profile reorder was blocked; no file was changed.";
            MessageBoxWarning(
                software_name,
                "Profile reorder is disabled because the legacy implementation deletes files before replacement. "
                "No profile or group file was changed.");
        }
    }

    void ProfileManager::SaveManager() {
        JsonStore::Save();
    }

    std::shared_ptr<ProxyEntity> ProfileManager::LoadProxyEntity(const QString &jsonPath) {
        // Load type
        ProxyEntity ent0(nullptr, nullptr);
        ent0.fn = jsonPath;
        auto validJson = ent0.Load();
        auto type = ent0.type;

        // Load content
        std::shared_ptr<ProxyEntity> ent;
        bool validType = validJson;

        if (validType) {
            ent = NewProxyEntity(type);
            validType = ent->bean->version != -114514;
            if (!validType) {
                recordRecoveryIssue(
                    jsonPath,
                    ent0.last_save_content,
                    QStringLiteral("Unsupported or unknown profile type: %1.").arg(type));
            }
        }

        if (validType) {
            ent->load_control_must = true;
            ent->fn = jsonPath;
            if (!ent->Load()) return nullptr;
        }
        return ent;
    }

    //  新建的不给 fn 和 id

    std::shared_ptr<ProxyEntity> ProfileManager::NewProxyEntity(const QString &type) {
        NekoGui_fmt::AbstractBean *bean;

        if (type == "socks") {
            bean = new NekoGui_fmt::SocksHttpBean(NekoGui_fmt::SocksHttpBean::type_Socks5);
        } else if (type == "http") {
            bean = new NekoGui_fmt::SocksHttpBean(NekoGui_fmt::SocksHttpBean::type_HTTP);
        } else if (type == "shadowsocks") {
            bean = new NekoGui_fmt::ShadowSocksBean();
        } else if (type == "chain") {
            bean = new NekoGui_fmt::ChainBean();
        } else if (type == "vmess") {
            bean = new NekoGui_fmt::VMessBean();
        } else if (type == "trojan") {
            bean = new NekoGui_fmt::TrojanVLESSBean(NekoGui_fmt::TrojanVLESSBean::proxy_Trojan);
        } else if (type == "vless") {
            bean = new NekoGui_fmt::TrojanVLESSBean(NekoGui_fmt::TrojanVLESSBean::proxy_VLESS);
        } else if (type == "hysteria2") {
            bean = new NekoGui_fmt::QUICBean(NekoGui_fmt::QUICBean::proxy_Hysteria2);
        } else if (type == "tuic") {
            bean = new NekoGui_fmt::QUICBean(NekoGui_fmt::QUICBean::proxy_TUIC);
        } else if (type == "anytls") {
            bean = new NekoGui_fmt::AnyTLSBean();
        } else if (type == "custom") {
            bean = new NekoGui_fmt::CustomBean();
        } else {
            bean = new NekoGui_fmt::AbstractBean(-114514);
        }

        auto ent = std::make_shared<ProxyEntity>(bean, type);
        return ent;
    }

    std::shared_ptr<Group> ProfileManager::NewGroup() {
        auto ent = std::make_shared<Group>();
        return ent;
    }

    // ProxyEntity

    ProxyEntity::ProxyEntity(NekoGui_fmt::AbstractBean *bean, const QString &type_) {
        if (type_ != nullptr) this->type = type_;

        _add(new configItem("type", &type, itemType::string));
        _add(new configItem("id", &id, itemType::integer));
        _add(new configItem("gid", &gid, itemType::integer));
        _add(new configItem("yc", &latency, itemType::integer));
        _add(new configItem("report", &full_test_report, itemType::string));

        // 可以不关联 bean，只加载 ProxyEntity 的信息
        if (bean != nullptr) {
            this->bean = std::shared_ptr<NekoGui_fmt::AbstractBean>(bean);
            // 有虚函数就要在这里 dynamic_cast
            _add(new configItem("bean", dynamic_cast<JsonStore *>(bean), itemType::jsonStore));
            _add(new configItem("traffic", dynamic_cast<JsonStore *>(traffic_data.get()), itemType::jsonStore));
        }
    };

    QString ProxyEntity::DisplayLatency() const {
        if (latency < 0) {
            return QObject::tr("Unavailable");
        } else if (latency > 0) {
            return UNICODE_LRO + QStringLiteral("%1 ms").arg(latency);
        } else {
            return "";
        }
    }

    QColor ProxyEntity::DisplayLatencyColor() const {
        if (latency < 0) {
            return Qt::red;
        } else if (latency > 0) {
            auto greenMs = dataStore->test_latency_url.startsWith("https://") ? 200 : 100;
            if (latency < greenMs) {
                return Qt::darkGreen;
            } else {
                return Qt::darkYellow;
            }
        } else {
            return {};
        }
    }

    // Profile

    int ProfileManager::NewProfileID() const {
        // profilesIdOrder also contains preserved, currently unreadable files.
        // Never reuse their IDs and overwrite them.
        if (profilesIdOrder.empty()) {
            return 0;
        }
        if (profilesIdOrder.last() == std::numeric_limits<int>::max()) return -1;
        return profilesIdOrder.last() + 1;
    }

    bool ProfileManager::AddProfile(const std::shared_ptr<ProxyEntity> &ent, int gid) {
        if (ent->id >= 0) {
            return false;
        }

        const auto previousGid = ent->gid;
        const auto previousFn = ent->fn;
        const auto newId = NewProfileID();
        if (newId < 0) {
            qCritical() << "Cannot allocate a profile ID without overwriting preserved data.";
            return false;
        }

        ent->gid = gid < 0 ? dataStore->current_group : gid;
        ent->id = newId;
        profiles[ent->id] = ent;
        profilesIdOrder.push_back(ent->id);

        ent->fn = QStringLiteral("profiles/%1.json").arg(ent->id);
        ent->Save();
        if (!ent->last_save_succeeded) {
            profiles.erase(ent->id);
            profilesIdOrder.removeAll(ent->id);
            ent->id = -1;
            ent->gid = previousGid;
            ent->fn = previousFn;
            return false;
        }
        return true;
    }

    bool ProfileManager::DeleteProfile(int id, const QString &reason, QString *error) {
        if (error != nullptr) error->clear();
        auto fail = [&](const QString &message) {
            if (error != nullptr) *error = message;
            qCritical() << message;
            return false;
        };

        if (id < 0) return fail(QStringLiteral("Profile deletion requires a non-negative id."));
        if (dataStore->started_id == id) {
            return fail(QStringLiteral("Profile %1 is running and cannot be deleted.").arg(id));
        }
        const auto profileIt = profiles.find(id);
        if (profileIt == profiles.end()) {
            return fail(QStringLiteral("Profile %1 is not loaded and cannot be deleted.").arg(id));
        }
        const auto profile = profileIt->second;

        for (const auto &[groupId, group]: groups) {
            if (group->front_proxy_id == id) {
                return fail(
                    QStringLiteral("Profile %1 is the front proxy of group %2; clear that reference explicitly first.")
                        .arg(id)
                        .arg(groupId));
            }
        }
        for (const auto &[otherId, other]: profiles) {
            if (otherId == id || other->type != "chain" || other->bean == nullptr) continue;
            if (other->ChainBean()->list.contains(id)) {
                return fail(
                    QStringLiteral("Profile %1 is referenced by chain profile %2; edit the chain explicitly first.")
                        .arg(id)
                        .arg(otherId));
            }
        }

        const auto profilePath = profile->fn;
        const auto operation = reason.trimmed().isEmpty()
                                 ? QStringLiteral("Profile deletion requested by the application.")
                                 : reason.trimmed();

        const auto previousAuxPorts = dataStore->aux_profile_ports;
        const auto previousAuxEntries = dataStore->aux_profile_port_entries;
        const auto removedAuxiliaryMapping = dataStore->aux_profile_ports.remove(id) > 0;
        const auto group = GetGroup(profile->gid);
        const auto previousGroupOrder = group != nullptr ? group->order : QList<int>{};
        const auto removedFromGroupOrder = group != nullptr && group->order.removeAll(id) > 0;

        auto restoreMemory = [&] {
            dataStore->aux_profile_ports = previousAuxPorts;
            dataStore->aux_profile_port_entries = previousAuxEntries;
            if (group != nullptr) group->order = previousGroupOrder;
        };

        QList<NekoGui_ConfigTransaction::FileMutation> mutations;
        QByteArray dataStoreAfter;
        QByteArray groupAfter;
        QString stagingError;
        if (removedAuxiliaryMapping) {
            if (!serializeStoreForTransaction(dataStore, &dataStoreAfter, &stagingError)) {
                restoreMemory();
                return fail(stagingError);
            }
            mutations.append({dataStore->fn, loadedStoreState(dataStore), {true, dataStoreAfter}});
        }
        if (removedFromGroupOrder) {
            if (!serializeStoreForTransaction(group.get(), &groupAfter, &stagingError)) {
                restoreMemory();
                return fail(stagingError);
            }
            mutations.append({group->fn, loadedStoreState(group.get()), {true, groupAfter}});
        }
        mutations.append({profilePath, {true, profile->last_save_content}, {false, {}}});

        const auto transaction = NekoGui_ConfigTransaction::Execute(operation, mutations);
        if (!transaction.succeeded()) {
            restoreMemory();
            return fail(
                QStringLiteral("Profile %1 deletion transaction did not commit; loaded data was preserved: %2")
                    .arg(id)
                    .arg(transaction.error));
        }

        if (removedAuxiliaryMapping) {
            dataStore->last_save_content = dataStoreAfter;
            dataStore->last_save_succeeded = true;
        }
        if (removedFromGroupOrder) {
            group->last_save_content = groupAfter;
            group->last_save_succeeded = true;
        }
        profiles.erase(id);
        profilesIdOrder.removeAll(id);
        qInfo() << "Profile deletion transaction committed:" << transaction.transactionPath;
        return true;
    }

    bool ProfileManager::MoveProfile(
        const std::shared_ptr<ProxyEntity> &ent,
        int gid,
        QString *error) {
        if (error != nullptr) error->clear();
        auto fail = [&](const QString &message) {
            if (error != nullptr) *error = message;
            qCritical() << message;
            return false;
        };

        if (ent == nullptr || ent->id < 0 || profiles.count(ent->id) == 0) {
            return fail(QStringLiteral("Profile move requires a loaded profile."));
        }
        if (gid < 0 || GetGroup(gid) == nullptr) {
            return fail(QStringLiteral("Profile %1 move target group %2 is not loaded.").arg(ent->id).arg(gid));
        }
        if (gid == ent->gid) return true;

        const auto oldGroup = GetGroup(ent->gid);
        const auto newGroup = GetGroup(gid);
        const auto previousOldOrder = oldGroup != nullptr ? oldGroup->order : QList<int>{};
        const auto previousNewOrder = newGroup->order;
        const auto previousGid = ent->gid;
        const auto removedFromOldOrder = oldGroup != nullptr && oldGroup->order.removeAll(ent->id) > 0;
        const auto addedToNewOrder = !newGroup->order.isEmpty() && !newGroup->order.contains(ent->id);
        if (addedToNewOrder) newGroup->order.append(ent->id);
        ent->gid = gid;

        auto restoreMemory = [&] {
            if (oldGroup != nullptr) oldGroup->order = previousOldOrder;
            newGroup->order = previousNewOrder;
            ent->gid = previousGid;
        };

        QList<NekoGui_ConfigTransaction::FileMutation> mutations;
        QByteArray oldGroupAfter;
        QByteArray newGroupAfter;
        QByteArray profileAfter;
        QString stagingError;
        if (removedFromOldOrder) {
            if (!serializeStoreForTransaction(oldGroup.get(), &oldGroupAfter, &stagingError)) {
                restoreMemory();
                return fail(stagingError);
            }
            mutations.append({oldGroup->fn, loadedStoreState(oldGroup.get()), {true, oldGroupAfter}});
        }
        if (addedToNewOrder) {
            if (!serializeStoreForTransaction(newGroup.get(), &newGroupAfter, &stagingError)) {
                restoreMemory();
                return fail(stagingError);
            }
            mutations.append({newGroup->fn, loadedStoreState(newGroup.get()), {true, newGroupAfter}});
        }
        if (!serializeStoreForTransaction(ent.get(), &profileAfter, &stagingError)) {
            restoreMemory();
            return fail(stagingError);
        }
        mutations.append({ent->fn, loadedStoreState(ent.get()), {true, profileAfter}});

        const auto transaction = NekoGui_ConfigTransaction::Execute(
            QStringLiteral("Explicit profile move from group %1 to group %2.").arg(previousGid).arg(gid),
            mutations);
        if (!transaction.succeeded()) {
            restoreMemory();
            return fail(
                QStringLiteral("Profile %1 move transaction did not commit; loaded data was preserved: %2")
                    .arg(ent->id)
                    .arg(transaction.error));
        }

        if (removedFromOldOrder) {
            oldGroup->last_save_content = oldGroupAfter;
            oldGroup->last_save_succeeded = true;
        }
        if (addedToNewOrder) {
            newGroup->last_save_content = newGroupAfter;
            newGroup->last_save_succeeded = true;
        }
        ent->last_save_content = profileAfter;
        ent->last_save_succeeded = true;
        qInfo() << "Profile move transaction committed:" << transaction.transactionPath;
        return true;
    }

    std::shared_ptr<ProxyEntity> ProfileManager::GetProfile(int id) {
        return profiles.count(id) ? profiles[id] : nullptr;
    }

    // Group

    Group::Group() {
        _add(new configItem("id", &id, itemType::integer));
        _add(new configItem("front_proxy_id", &front_proxy_id, itemType::integer));
        _add(new configItem("archive", &archive, itemType::boolean));
        _add(new configItem("skip_auto_update", &skip_auto_update, itemType::boolean));
        _add(new configItem("name", &name, itemType::string));
        _add(new configItem("order", &order, itemType::integerList));
        _add(new configItem("url", &url, itemType::string));
        _add(new configItem("info", &info, itemType::string));
        _add(new configItem("lastup", &sub_last_update, itemType::integer64));
        _add(new configItem("source_type", &source_type, itemType::string));
        _add(new configItem("default_client_mode", &default_client_mode, itemType::string));
        _add(new configItem("default_client_value", &default_client_value, itemType::string));
        _add(new configItem("default_client_source", &default_client_source, itemType::string));
        _add(new configItem("default_server_resolver_doh", &default_server_resolver_doh, itemType::string));
        _add(new configItem("default_server_resolver_source", &default_server_resolver_source, itemType::string));
        _add(new configItem("default_server_resolver_fallback", &default_server_resolver_allow_local_fallback, itemType::boolean));
        _add(new configItem("manually_column_width", &manually_column_width, itemType::boolean));
        _add(new configItem("column_width", &column_width, itemType::integerList));
    }

    bool Group::DefaultClientManagedBySubscription() const {
        const auto source = default_client_source.trimmed().toLower();
        if (source == "manual") return false;
        if (source == "subscription") return true;

        const auto sourceType = source_type.trimmed().toLower();
        const auto mode = default_client_mode.trimmed().toLower();
        if (sourceType == "clash") {
            return mode.isEmpty() || mode == "mihomo";
        }
        return false;
    }

    bool Group::DefaultResolverManagedBySubscription() const {
        const auto source = default_server_resolver_source.trimmed().toLower();
        if (source == "manual") return false;
        if (source == "subscription") return true;

        const auto sourceType = source_type.trimmed().toLower();
        return sourceType == "clash";
    }

    void Group::SetDefaultClientManagedBySubscription(bool enabled) {
        default_client_source = enabled ? "subscription" : "manual";
    }

    void Group::SetDefaultResolverManagedBySubscription(bool enabled) {
        default_server_resolver_source = enabled ? "subscription" : "manual";
    }

    std::shared_ptr<Group> ProfileManager::LoadGroup(const QString &jsonPath) {
        auto ent = std::make_shared<Group>();
        ent->fn = jsonPath;
        if (!ent->Load()) return nullptr;
        return ent;
    }

    int ProfileManager::NewGroupID() const {
        // groupsIdOrder also contains preserved, currently unreadable files.
        if (groupsIdOrder.empty()) {
            return 0;
        }
        if (groupsIdOrder.last() == std::numeric_limits<int>::max()) return -1;
        return groupsIdOrder.last() + 1;
    }

    bool ProfileManager::AddGroup(const std::shared_ptr<Group> &ent) {
        if (ent->id >= 0) {
            return false;
        }

        const auto previousFn = ent->fn;
        const auto newId = NewGroupID();
        if (newId < 0) {
            qCritical() << "Cannot allocate a group ID without overwriting preserved data.";
            return false;
        }

        ent->id = newId;
        groups[ent->id] = ent;
        groupsIdOrder.push_back(ent->id);
        groupsTabOrder.push_back(ent->id);

        ent->fn = QStringLiteral("groups/%1.json").arg(ent->id);
        ent->Save();
        if (!ent->last_save_succeeded) {
            groups.erase(ent->id);
            groupsIdOrder.removeAll(ent->id);
            groupsTabOrder.removeAll(ent->id);
            ent->id = -1;
            ent->fn = previousFn;
            return false;
        }
        return true;
    }

    bool ProfileManager::DeleteGroup(int gid, const QString &reason, QString *error) {
        if (error != nullptr) error->clear();
        auto fail = [&](const QString &message) {
            if (error != nullptr) *error = message;
            qCritical() << message;
            return false;
        };

        if (groups.size() <= 1) return fail(QStringLiteral("The last remaining group cannot be deleted."));
        const auto groupIt = groups.find(gid);
        if (groupIt == groups.end()) {
            return fail(QStringLiteral("Group %1 is not loaded and cannot be deleted.").arg(gid));
        }

        QList<int> toDelete;
        for (const auto &[id, profile]: profiles) {
            if (profile->gid == gid) toDelete += id; // map访问中，不能操作
        }
        if (!toDelete.isEmpty()) {
            return fail(
                QStringLiteral(
                    "Group %1 still contains %2 profile(s). Batch group deletion is blocked until the "
                    "batch group deletion transaction is implemented; delete or move the profiles explicitly first.")
                    .arg(gid)
                    .arg(toDelete.size()));
        }

        const auto group = groupIt->second;
        const auto groupPath = group->fn;
        const auto operation = reason.trimmed().isEmpty()
                                 ? QStringLiteral("Empty group deletion requested by the application.")
                                 : reason.trimmed();

        const auto previousOrder = groupsTabOrder;
        const auto removedFromOrder = groupsTabOrder.removeAll(gid) > 0;

        const auto previousCurrentGroup = dataStore->current_group;
        const bool changesCurrentGroup = previousCurrentGroup == gid;
        if (changesCurrentGroup) {
            for (const auto &[candidateId, candidate]: groups) {
                (void) candidate;
                if (candidateId != gid) {
                    dataStore->current_group = candidateId;
                    break;
                }
            }
        }

        auto restoreMemory = [&] {
            groupsTabOrder = previousOrder;
            dataStore->current_group = previousCurrentGroup;
        };

        QList<NekoGui_ConfigTransaction::FileMutation> mutations;
        QByteArray managerAfter;
        QByteArray dataStoreAfter;
        QString stagingError;
        if (removedFromOrder) {
            if (!serializeStoreForTransaction(this, &managerAfter, &stagingError)) {
                restoreMemory();
                return fail(stagingError);
            }
            mutations.append({fn, loadedStoreState(this), {true, managerAfter}});
        }
        if (changesCurrentGroup) {
            if (!serializeStoreForTransaction(dataStore, &dataStoreAfter, &stagingError)) {
                restoreMemory();
                return fail(stagingError);
            }
            mutations.append({dataStore->fn, loadedStoreState(dataStore), {true, dataStoreAfter}});
        }
        mutations.append({groupPath, {true, group->last_save_content}, {false, {}}});

        const auto transaction = NekoGui_ConfigTransaction::Execute(operation, mutations);
        if (!transaction.succeeded()) {
            restoreMemory();
            return fail(
                QStringLiteral("Group %1 deletion transaction did not commit; loaded data was preserved: %2")
                    .arg(gid)
                    .arg(transaction.error));
        }

        if (removedFromOrder) {
            last_save_content = managerAfter;
            last_save_succeeded = true;
        }
        if (changesCurrentGroup) {
            dataStore->last_save_content = dataStoreAfter;
            dataStore->last_save_succeeded = true;
        }

        groups.erase(gid);
        groupsIdOrder.removeAll(gid);
        qInfo() << "Group deletion transaction committed:" << transaction.transactionPath;
        return true;
    }

    std::shared_ptr<Group> ProfileManager::GetGroup(int id) {
        return groups.count(id) ? groups[id] : nullptr;
    }

    std::shared_ptr<Group> ProfileManager::CurrentGroup() {
        return GetGroup(dataStore->current_group);
    }

    QList<std::shared_ptr<ProxyEntity>> Group::Profiles() const {
        QList<std::shared_ptr<ProxyEntity>> ret;
        for (const auto &[_, profile]: profileManager->profiles) {
            if (id == profile->gid) ret += profile;
        }
        return ret;
    }

    QList<std::shared_ptr<ProxyEntity>> Group::ProfilesWithOrder() const {
        if (order.isEmpty()) {
            return Profiles();
        } else {
            QList<std::shared_ptr<ProxyEntity>> ret;
            for (auto _id: order) {
                auto ent = profileManager->GetProfile(_id);
                if (ent != nullptr) ret += ent;
            }
            return ret;
        }
    }

} // namespace NekoGui
