#pragma once

#include <QMainWindow>

#include <cstdint>

#include "main/NekoGui.hpp"
#include "main/RuntimeTransition.hpp"

#ifndef MW_INTERFACE

#include <QTime>
#include <QTableWidgetItem>
#include <QKeyEvent>
#include <QSystemTrayIcon>
#include <QProcess>
#include <QTextDocument>
#include <QShortcut>
#include <QSemaphore>
#include <QMutex>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include "GroupSort.hpp"

#include "db/ProxyEntity.hpp"
#include "main/GuiUtils.hpp"
#endif

namespace NekoGui_sys {
    class CoreProcess;
}

QT_BEGIN_NAMESPACE
namespace Ui {
    class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    ~MainWindow() override;

    void refresh_proxy_list(const int &id = -1);

    void show_group(int gid);

    void refresh_groups();

    void refresh_status(const QString &traffic_update = "");

    bool isInternalTunActive() const;

    bool hasActiveIsolatedTest();

    enum class CoreStartReason {
        UserAction,
        ProfileReload,
        EnableInternalTun,
        DisableInternalTun,
        CoreCrashRecovery,
    };

    enum class CoreStopReason {
        UserAction,
        ProfileReload,
        AppExit,
        EnableInternalTun,
        CoreCrashCleanup,
        DisableInternalTun,
    };

    enum class ProxyModeChangeReason {
        UserAction,
        VpnProcessExit,
    };

    void neko_start(int _id = -1, CoreStartReason reason = CoreStartReason::UserAction,
                    std::uint64_t expectedDaemonGeneration = 0,
                    std::uint64_t expectedRequestGeneration = 0);

    void neko_stop(bool crash = false, bool sem = false,
                   CoreStopReason reason = CoreStopReason::UserAction,
                   bool nestedTransition = false,
                   const std::shared_ptr<std::atomic_bool>& completionResult = {},
                   const NekoGui_Runtime::TransitionTicket& ownerTransition = {},
                   std::uint64_t expectedCrashDaemonGeneration = 0);

    void neko_set_spmode_system_proxy(bool enable, bool save = true, ProxyModeChangeReason reason = ProxyModeChangeReason::UserAction);

    void neko_set_spmode_vpn(bool enable, bool save = true, ProxyModeChangeReason reason = ProxyModeChangeReason::UserAction);

    void show_log_impl(const QString &log);

    void start_select_mode(QObject *context, const std::function<void(int)> &callback);

    void refresh_connection_list(const QJsonArray &arr);

    void RegisterHotkey(bool unregister);

    bool StopVPNProcess(bool unconditional = false);

signals:

    void profile_selected(int id);

public slots:

    void on_commitDataRequest();

    void on_menu_exit_triggered();

#ifndef MW_INTERFACE

private slots:

    void on_masterLogBrowser_customContextMenuRequested(const QPoint &pos);

    void on_menu_basic_settings_triggered();

    void on_menu_routing_settings_triggered();

    void on_menu_vpn_settings_triggered();

    void on_menu_hotkey_settings_triggered();

    void on_menu_add_from_input_triggered();

    void on_menu_add_from_clipboard_triggered();

    void on_menu_clone_triggered();

    void on_menu_move_triggered();

    void on_menu_delete_triggered();

    void on_menu_start_auxiliary_triggered();

    void on_menu_stop_auxiliary_triggered();

    void on_menu_copy_auxiliary_proxy_triggered();

    void on_menu_reset_traffic_triggered();

    void on_menu_profile_debug_info_triggered();

    void on_menu_copy_links_triggered();

    void on_menu_copy_links_without_remarks_triggered();

    void on_menu_copy_ip_port_user_pass_triggered();

    void on_menu_copy_links_nkr_triggered();

    void on_menu_export_config_triggered();

    void display_qr_link(bool nkrFormat = false);

    void on_menu_scan_qr_triggered();

    void on_menu_clear_test_result_triggered();

    void on_menu_manage_groups_triggered();

    void on_menu_select_all_triggered();

    void on_menu_delete_repeat_triggered();

    void on_menu_remove_unavailable_triggered();

    void on_menu_update_subscription_triggered();

    void on_menu_resolve_domain_triggered();

    void on_proxyListTable_itemDoubleClicked(QTableWidgetItem *item);

    void on_proxyListTable_customContextMenuRequested(const QPoint &pos);

    void on_tabWidget_currentChanged(int index);

private:
    Ui::MainWindow *ui;
    QSystemTrayIcon *tray;
    QShortcut *shortcut_ctrl_f = new QShortcut(QKeySequence("Ctrl+F"), this);
    QShortcut *shortcut_esc = new QShortcut(QKeySequence("Esc"), this);
    //
    NekoGui_sys::CoreProcess *core_process = nullptr;
    qint64 vpn_pid = 0;
    //
    bool qvLogAutoScoll = true;
    QTextDocument *qvLogDocument = new QTextDocument(this);
    //
    QString title_error;
    int icon_status = -1;
    std::shared_ptr<NekoGui::ProxyEntity> running;
    QString traffic_update_cache;
    QTime last_test_time;
    //
    int proxy_last_order = -1;
    bool select_mode = false;
    QMutex mu_exit;
    QSemaphore sem_stopped;
    NekoGui_Runtime::TransitionCoordinator runtime_transition;
    std::mutex pending_core_crash_mutex;
    std::set<std::uint64_t> pending_core_crash_generations;
    std::mutex pending_profile_start_mutex;
    std::optional<NekoGui_Runtime::DaemonProfileStartRequest> pending_profile_start;
    bool pending_profile_start_dispatch_queued = false;
    std::map<QString, int> pending_core_handshake_attempts;
    int exit_reason = 0;
    int exit_had_profile_id = -1919;
    bool running_internal_tun = false;
    std::uint64_t running_generation = 0;
    std::uint64_t running_daemon_generation = 0;
    QString running_daemon_instance_id;
    QByteArray running_config_sha256;
    bool runtime_state_indeterminate = false;
    QString runtime_state_indeterminate_reason;

    QList<std::shared_ptr<NekoGui::ProxyEntity>> get_now_selected_list();

    QList<std::shared_ptr<NekoGui::ProxyEntity>> get_selected_or_group();

    void dialog_message_impl(const QString &sender, const QString &info);

    void finish_runtime_transition(const NekoGui_Runtime::TransitionTicket& transition);

    void queue_core_crash_cleanup(std::uint64_t daemonGeneration);

    void dispatch_core_crash_cleanup(
        const NekoGui_Runtime::TransitionTicket& transition,
        std::set<std::uint64_t> daemonGenerations);

    void queue_daemon_profile_start(
        const NekoGui_Runtime::DaemonProfileStartRequest& request);

    void dispatch_pending_daemon_profile_start();

    void clear_pending_daemon_profile_start(
        const NekoGui_Runtime::DaemonProfileStartRequest& request);

    void refresh_proxy_list_impl(const int &id = -1, GroupSortAction groupSortAction = {});

    void refresh_proxy_list_impl_refresh_data(const int &id = -1);

    void keyPressEvent(QKeyEvent *event) override;

    void closeEvent(QCloseEvent *event) override;

    //

    void HotkeyEvent(const QString &key);

    bool StartVPNProcess();

    // grpc and ...

    static void setup_grpc();

    void speedtest_current_group(int mode, bool test_group);

    void speedtest_current();

    static bool stop_core_daemon(QString* detail = nullptr);

    void CheckUpdate();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

#endif // MW_INTERFACE
};

inline MainWindow *GetMainWindow() {
    return (MainWindow *) mainwindow;
}

void UI_InitMainWindow();
