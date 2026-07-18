#include "./ui_mainwindow.h"
#include "mainwindow.h"

#include "db/Database.hpp"
#include "db/ConfigBuilder.hpp"
#include "db/traffic/TrafficLooper.hpp"
#include "rpc/gRPC.h"
#include "ui/widget/MessageBoxTimer.h"

#include <QTimer>
#include <QThread>
#include <QInputDialog>
#include <QPushButton>
#include <QDesktopServices>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QSet>

#include <algorithm>
#include <atomic>

// ext core

std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> CreateExtCFromExtR(const std::list<std::shared_ptr<NekoGui_fmt::ExternalBuildResult>> &extRs, bool start) {
    // plz run and start in same thread
    std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> l;
    for (const auto &extR: extRs) {
        std::shared_ptr<NekoGui_sys::ExternalProcess> extC(new NekoGui_sys::ExternalProcess());
        extC->tag = extR->tag;
        extC->program = extR->program;
        extC->arguments = extR->arguments;
        extC->env = extR->env;
        l.emplace_back(extC);
        //
        if (start) extC->Start();
    }
    return l;
}

// grpc

#ifndef NKR_NO_GRPC
using namespace NekoGui_rpc;
#endif

namespace {
    bool internalTunWouldBeInterrupted() {
        return GetMainWindow() != nullptr && GetMainWindow()->isInternalTunActive();
    }

    bool tunModeChangePendingWhileRunning() {
        return NekoGui::dataStore->spmode_vpn &&
               NekoGui::dataStore->started_id >= 0 &&
               GetMainWindow() != nullptr &&
               NekoGui::dataStore->vpn_internal_tun != GetMainWindow()->isInternalTunActive();
    }

    QString internalTunReloadBlockedMessage() {
        return QObject::tr("Internal Tun is running. This operation would stop and restart sing-box and may restore direct traffic. Disable Tun explicitly first.");
    }

    QString tunModeChangeBlockedMessage() {
        return QObject::tr("Tun implementation was changed while Tun is running. Disable Tun explicitly before restarting or reloading.");
    }
}

void MainWindow::setup_grpc() {
#ifndef NKR_NO_GRPC
    // Setup Connection
    defaultClient = new Client(
        [=](const QString &errStr) {
            MW_show_log("[Error] gRPC: " + errStr);
        },
        "127.0.0.1:" + Int2String(NekoGui::dataStore->core_port), NekoGui::dataStore->core_token);

    // Looper
    runOnNewThread([=] { NekoGui_traffic::trafficLooper->Loop(); });
#endif
}

// 测速

namespace {
    QMutex speedtestingMutex;
    bool speedtesting = false;
    QList<QThread *> speedtestingThreads;
    std::shared_ptr<std::atomic_bool> speedtestingCancelToken;

    QMutex runningLabelUrlTestMutex;
    bool runningLabelUrlTesting = false;
    quint64 runningLabelUrlTestGeneration = 0;

    bool beginSpeedtesting(std::shared_ptr<std::atomic_bool> *token) {
        QMutexLocker locker(&speedtestingMutex);
        if (speedtesting) return false;
        speedtesting = true;
        speedtestingThreads.clear();
        speedtestingCancelToken = std::make_shared<std::atomic_bool>(false);
        if (token != nullptr) *token = speedtestingCancelToken;
        return true;
    }

    bool cancelSpeedtesting(const std::shared_ptr<std::atomic_bool> &expectedToken = {}) {
        QList<QThread *> threadsToStop;
        {
            QMutexLocker locker(&speedtestingMutex);
            if (!speedtesting && speedtestingCancelToken == nullptr) return false;
            if (expectedToken != nullptr && speedtestingCancelToken != expectedToken) return false;
            if (speedtestingCancelToken != nullptr) speedtestingCancelToken->store(true);
            threadsToStop = speedtestingThreads;
            speedtesting = false;
            speedtestingThreads.clear();
            speedtestingCancelToken.reset();
        }
        for (auto *thread: threadsToStop) {
            if (thread == nullptr) continue;
            thread->requestInterruption();
            thread->quit();
            thread->exit();
        }
        return true;
    }

    bool finishSpeedtesting(const std::shared_ptr<std::atomic_bool> &token) {
        QMutexLocker locker(&speedtestingMutex);
        if (speedtestingCancelToken != token) return false;
        speedtesting = false;
        speedtestingThreads.clear();
        speedtestingCancelToken.reset();
        return true;
    }

    void registerSpeedtestingThread(QThread *thread) {
        QMutexLocker locker(&speedtestingMutex);
        if (thread != nullptr && !speedtestingThreads.contains(thread)) speedtestingThreads << thread;
    }

    void unregisterSpeedtestingThread(QThread *thread) {
        QMutexLocker locker(&speedtestingMutex);
        speedtestingThreads.removeAll(thread);
    }

    quint64 beginRunningLabelUrlTest() {
        QMutexLocker locker(&runningLabelUrlTestMutex);
        if (runningLabelUrlTesting) return 0;
        runningLabelUrlTesting = true;
        return ++runningLabelUrlTestGeneration;
    }

    bool finishRunningLabelUrlTest(quint64 generation) {
        QMutexLocker locker(&runningLabelUrlTestMutex);
        if (!runningLabelUrlTesting || runningLabelUrlTestGeneration != generation) return false;
        runningLabelUrlTesting = false;
        return true;
    }
} // namespace

void MainWindow::speedtest_current_group(int mode, bool test_group) {
    // menu_stop_testing
    if (mode == 114514) {
        if (cancelSpeedtesting()) {
            MW_show_log(QObject::tr("Speedtest stop requested."));
        }
        return;
    }

    auto profiles = get_selected_or_group();
    if (test_group) profiles = NekoGui::profileManager->CurrentGroup()->ProfilesWithOrder();
    if (profiles.isEmpty()) return;
    auto group = NekoGui::profileManager->CurrentGroup();
    if (group->archive) return;

#ifndef NKR_NO_GRPC
    QStringList full_test_flags;
    if (mode == libcore::FullTest) {
        auto w = new QDialog(this);
        auto layout = new QVBoxLayout(w);
        w->setWindowTitle(tr("Test Options"));
        //
        auto l1 = new QCheckBox(tr("Latency"));
        auto l2 = new QCheckBox(tr("UDP latency"));
        auto l3 = new QCheckBox(tr("Download speed"));
        auto l4 = new QCheckBox(tr("In and Out IP"));
        //
        auto box = new QDialogButtonBox;
        box->setOrientation(Qt::Horizontal);
        box->setStandardButtons(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
        connect(box, &QDialogButtonBox::accepted, w, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, w, &QDialog::reject);
        //
        layout->addWidget(l1);
        layout->addWidget(l2);
        layout->addWidget(l3);
        layout->addWidget(l4);
        layout->addWidget(box);
        if (w->exec() != QDialog::Accepted) {
            w->deleteLater();
            return;
        }
        //
        if (l1->isChecked()) full_test_flags << "1";
        if (l2->isChecked()) full_test_flags << "2";
        if (l3->isChecked()) full_test_flags << "3";
        if (l4->isChecked()) full_test_flags << "4";
        //
        w->deleteLater();
        if (full_test_flags.isEmpty()) return;
    }
    std::shared_ptr<std::atomic_bool> cancelToken;
    if (!beginSpeedtesting(&cancelToken)) {
        MessageBoxWarning(software_name, QObject::tr("The last speed test did not exit completely, please wait. If it persists, use Stop Testing."));
        return;
    }

    const int profileCount = static_cast<int>(profiles.size());
    const int configuredConcurrency = std::max(1, NekoGui::dataStore->test_concurrent);
    const int batches = std::max(1, (profileCount + configuredConcurrency - 1) / configuredConcurrency);
    const int perBatchTimeoutSeconds = mode == libcore::FullTest
                                           ? std::max(20, NekoGui::dataStore->test_download_timeout + 15)
                                           : 20;
    const int watchdogMs = std::clamp(batches * perBatchTimeoutSeconds * 1000, 30000, 10 * 60 * 1000);
    setTimeout([cancelToken] {
        if (cancelSpeedtesting(cancelToken)) {
            MW_show_log(QObject::tr("Speedtest timed out and its busy state was reset."));
        }
    }, this, watchdogMs);

    runOnNewThread([this, profiles, mode, full_test_flags, cancelToken]() {
        QMutex lock_write;
        QSemaphore doneSem;
        auto profiles_test = profiles; // copy
        const int workerProfileCount = static_cast<int>(profiles_test.size());
        int threadN = std::min(std::max(1, NekoGui::dataStore->test_concurrent), std::max(1, workerProfileCount));

        // Threads
        for (int i = 0; i < threadN; i++) {
            runOnNewThread([&, cancelToken] {
                auto *thread = QObject::thread();
                registerSpeedtestingThread(thread);

                forever {
                    if (cancelToken->load()) break;
                    //
                    std::shared_ptr<NekoGui::ProxyEntity> profile;
                    lock_write.lock();
                    if (!profiles_test.isEmpty() && !cancelToken->load()) {
                        profile = profiles_test.takeFirst();
                    }
                    lock_write.unlock();
                    if (profile == nullptr) break;

                    //
                    libcore::TestReq req;
                    req.set_mode((libcore::TestMode) mode);
                    req.set_timeout(10 * 1000);
                    req.set_url(NekoGui::dataStore->test_latency_url.toStdString());

                    //
                    std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> extCs;
                    QSemaphore extSem;

                    if (mode == libcore::TestMode::UrlTest || mode == libcore::FullTest) {
                        auto c = BuildConfig(profile, true, false);
                        if (!c->error.isEmpty()) {
                            profile->full_test_report = c->error;
                            profile->Save();
                            auto profileId = profile->id;
                            runOnUiThread([this, profileId] {
                                refresh_proxy_list(profileId);
                            });
                            continue;
                        }
                        //
                        if (!c->extRs.empty()) {
                            runOnUiThread(
                                [&] {
                                    extCs = CreateExtCFromExtR(c->extRs, true);
                                    QThread::msleep(500);
                                    extSem.release();
                                },
                                DS_cores);
                            extSem.acquire();
                        }
                        //
                        auto config = new libcore::LoadConfigReq;
                        config->set_core_config(QJsonObject2QString(c->coreConfig, false).toStdString());
                        req.set_allocated_config(config);
                        req.set_in_address(profile->bean->serverAddress.toStdString());

                        req.set_full_latency(full_test_flags.contains("1"));
                        req.set_full_udp_latency(full_test_flags.contains("2"));
                        req.set_full_speed(full_test_flags.contains("3"));
                        req.set_full_in_out(full_test_flags.contains("4"));

                        req.set_full_speed_url(NekoGui::dataStore->test_download_url.toStdString());
                        req.set_full_speed_timeout(NekoGui::dataStore->test_download_timeout);
                    } else if (mode == libcore::TcpPing) {
                        req.set_address(profile->bean->DisplayAddress().toStdString());
                    }

                    bool rpcOK;
                    auto result = defaultClient->Test(&rpcOK, req);
                    //
                    if (!extCs.empty()) {
                        runOnUiThread(
                            [&] {
                                for (const auto &extC: extCs) {
                                    extC->Kill();
                                }
                                extSem.release();
                            },
                            DS_cores);
                        extSem.acquire();
                    }
                    //
                    if (!rpcOK) {
                        profile->latency = -1;
                        profile->full_test_report = QObject::tr("gRPC test failed.");
                        profile->Save();
                        auto profileId = profile->id;
                        runOnUiThread([this, profileId] {
                            refresh_proxy_list(profileId);
                        });
                        continue;
                    }

                    if (result.error().empty()) {
                        profile->latency = result.ms();
                        if (profile->latency == 0) profile->latency = 1; // nekoray use 0 to represents not tested
                    } else {
                        profile->latency = -1;
                    }
                    profile->full_test_report = result.full_report().c_str(); // higher priority
                    profile->Save();

                    if (!result.error().empty()) {
                        MW_show_log(tr("[%1] test error: %2").arg(profile->bean->DisplayTypeAndName(), result.error().c_str()));
                    }

                    auto profileId = profile->id;
                    runOnUiThread([this, profileId] {
                        refresh_proxy_list(profileId);
                    });
                }
                unregisterSpeedtestingThread(thread);
                doneSem.release();
            });
        }

        // Control
        doneSem.acquire(threadN);
        const auto cancelled = cancelToken->load();
        if (finishSpeedtesting(cancelToken)) {
            runOnUiThread([cancelled] {
                MW_show_log(cancelled ? QObject::tr("Speedtest stopped.") : QObject::tr("Speedtest finished."));
            });
        }
    });
#endif
}

void MainWindow::speedtest_current() {
#ifndef NKR_NO_GRPC
    const auto generation = beginRunningLabelUrlTest();
    if (generation == 0) {
        MW_show_log(tr("UrlTest is already running."));
        return;
    }
    last_test_time = QTime::currentTime();
    ui->label_running->setText(tr("Testing"));
    setTimeout([this, generation] {
        if (!finishRunningLabelUrlTest(generation)) return;
        last_test_time = QTime::currentTime();
        ui->label_running->setText(tr("Test Result") + ": " + tr("Unavailable"));
        MW_show_log(tr("UrlTest timed out."));
    }, this, 15000);

    runOnNewThread([=] {
        libcore::TestReq req;
        req.set_mode(libcore::UrlTest);
        req.set_timeout(10 * 1000);
        req.set_url(NekoGui::dataStore->test_latency_url.toStdString());

        bool rpcOK;
        auto result = defaultClient->Test(&rpcOK, req);

        auto latency = result.ms();

        runOnUiThread([=] {
            if (!finishRunningLabelUrlTest(generation)) return;
            last_test_time = QTime::currentTime();
            if (!rpcOK) {
                MW_show_log(tr("UrlTest error: gRPC test failed."));
                ui->label_running->setText(tr("Test Result") + ": " + tr("Unavailable"));
                return;
            }
            if (!result.error().empty()) {
                MW_show_log(QStringLiteral("UrlTest error: %1").arg(result.error().c_str()));
            }
            if (latency <= 0) {
                ui->label_running->setText(tr("Test Result") + ": " + tr("Unavailable"));
            } else if (latency > 0) {
                ui->label_running->setText(tr("Test Result") + ": " + QStringLiteral("%1 ms").arg(latency));
            }
        });
    });
#endif
}

void MainWindow::stop_core_daemon() {
#ifndef NKR_NO_GRPC
    NekoGui_rpc::defaultClient->Exit();
#endif
}

void MainWindow::neko_start(int _id, CoreStartReason reason) {
    if (NekoGui::dataStore->prepare_exit) return;

    auto ents = get_now_selected_list();
    auto ent = (_id < 0 && !ents.isEmpty()) ? ents.first() : NekoGui::profileManager->GetProfile(_id);
    if (ent == nullptr) return;

    if (select_mode) {
        emit profile_selected(ent->id);
        select_mode = false;
        refresh_status();
        return;
    }

    auto group = NekoGui::profileManager->GetGroup(ent->gid);
    if (group == nullptr || group->archive) return;

    if (internalTunWouldBeInterrupted() &&
        reason != CoreStartReason::EnableInternalTun &&
        reason != CoreStartReason::DisableInternalTun &&
        reason != CoreStartReason::CoreCrashRecovery) {
        MessageBoxWarning(software_name, internalTunReloadBlockedMessage());
        return;
    }
    if (tunModeChangePendingWhileRunning() &&
        reason != CoreStartReason::EnableInternalTun &&
        reason != CoreStartReason::DisableInternalTun &&
        reason != CoreStartReason::CoreCrashRecovery) {
        MessageBoxWarning(software_name, tunModeChangeBlockedMessage());
        return;
    }

    auto result = BuildConfig(ent, false, false);
    if (!result->error.isEmpty()) {
        MessageBoxWarning("BuildConfig return error", result->error);
        return;
    }
    if (NekoGui::dataStore->aux_profile_ports.remove(ent->id) > 0) {
        NekoGui::dataStore->Save();
    }

    auto neko_start_stage2 = [=] {
#ifndef NKR_NO_GRPC
        libcore::LoadConfigReq req;
        req.set_core_config(QJsonObject2QString(result->coreConfig, false).toStdString());
        req.set_enable_nekoray_connections(NekoGui::dataStore->connection_statistics);
        if (NekoGui::dataStore->traffic_loop_interval > 0) {
            QSet<QString> statsTags;
            for (const auto &item: result->outboundStats) {
                const auto tag = QString::fromStdString(item->tag);
                if (!tag.isEmpty()) statsTags.insert(tag);
            }
            statsTags.insert("proxy");
            for (const auto &tag: statsTags) {
                req.add_stats_outbounds(tag.toStdString());
            }
            req.add_stats_outbounds("bypass");
        }
        //
        bool rpcOK;
        QString error = defaultClient->Start(&rpcOK, req);
        if (rpcOK && !error.isEmpty()) {
            runOnUiThread([=] { MessageBoxWarning("LoadConfig return error", error); });
            return false;
        } else if (!rpcOK) {
            return false;
        }
        //
        NekoGui_traffic::trafficLooper->proxy = result->outboundStat.get();
        NekoGui_traffic::trafficLooper->items = result->outboundStats;
        NekoGui::dataStore->ignoreConnTag = result->ignoreConnTag;
        NekoGui_traffic::trafficLooper->loop_enabled = true;
#endif

        runOnUiThread(
            [=] {
                auto extCs = CreateExtCFromExtR(result->extRs, true);
                NekoGui_sys::running_ext.splice(NekoGui_sys::running_ext.end(), extCs);
            },
            DS_cores);

        NekoGui::dataStore->UpdateStartedId(ent->id);
        running_internal_tun = NekoGui::dataStore->vpn_internal_tun && NekoGui::dataStore->spmode_vpn;
        running = ent;

        runOnUiThread([=] {
            refresh_status();
            refresh_proxy_list(ent->id);
        });

        return true;
    };

    if (!mu_starting.tryLock()) {
        MessageBoxWarning(software_name, "Another profile is starting...");
        return;
    }
    if (!mu_stopping.tryLock()) {
        MessageBoxWarning(software_name, "Another profile is stopping...");
        mu_starting.unlock();
        return;
    }
    mu_stopping.unlock();

    // check core state
    if (!NekoGui::dataStore->core_running) {
        runOnUiThread(
            [=] {
                MW_show_log("Try to start the config, but the core has not listened to the grpc port, so restart it...");
                core_process->start_profile_when_core_is_up = ent->id;
                core_process->Restart();
            },
            DS_cores);
        mu_starting.unlock();
        return; // let CoreProcess call neko_start when core is up
    }

    // timeout message
    auto restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                                         QMessageBox::Yes | QMessageBox::No, this);
    connect(restartMsgbox, &QMessageBox::accepted, this, [=] { MW_dialog_message("", "RestartProgram"); });
    auto restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 5000);

    runOnNewThread([=] {
        // stop current running
        if (NekoGui::dataStore->started_id >= 0) {
            auto stopReason = CoreStopReason::ProfileReload;
            auto stopCrash = false;
            if (reason == CoreStartReason::EnableInternalTun) stopReason = CoreStopReason::EnableInternalTun;
            if (reason == CoreStartReason::DisableInternalTun) stopReason = CoreStopReason::DisableInternalTun;
            if (reason == CoreStartReason::CoreCrashRecovery) {
                stopReason = CoreStopReason::CoreCrashCleanup;
                stopCrash = true;
            }
            runOnUiThread([=] { neko_stop(stopCrash, true, stopReason); });
            sem_stopped.acquire();
        }
        // do start
        MW_show_log(">>>>>>>> " + tr("Starting profile %1").arg(ent->bean->DisplayTypeAndName()));
        if (!neko_start_stage2()) {
            MW_show_log("<<<<<<<< " + tr("Failed to start profile %1").arg(ent->bean->DisplayTypeAndName()));
        }
        mu_starting.unlock();
        // cancel timeout
        runOnUiThread([=] {
            restartMsgboxTimer->cancel();
            restartMsgboxTimer->deleteLater();
            restartMsgbox->deleteLater();
#ifdef Q_OS_LINUX
            // Check systemd-resolved
            if (NekoGui::dataStore->spmode_vpn && NekoGui::dataStore->routing->direct_dns.startsWith("local") && ReadFileText("/etc/resolv.conf").contains("systemd-resolved")) {
                MW_show_log("[Warning] The default Direct DNS may not works with systemd-resolved, you may consider change your DNS settings.");
            }
#endif
        });
    });
}

void MainWindow::neko_stop(bool crash, bool sem, CoreStopReason reason) {
    auto id = NekoGui::dataStore->started_id;
    if (id < 0) {
        if (sem) sem_stopped.release();
        return;
    }

    if (!crash && internalTunWouldBeInterrupted() &&
        reason != CoreStopReason::EnableInternalTun &&
        reason != CoreStopReason::DisableInternalTun) {
        if (sem) sem_stopped.release();
        MessageBoxWarning(software_name, internalTunReloadBlockedMessage());
        return;
    }

    auto neko_stop_stage2 = [=] {
        runOnUiThread(
            [=] {
                while (!NekoGui_sys::running_ext.empty()) {
                    auto extC = NekoGui_sys::running_ext.front();
                    extC->Kill();
                    NekoGui_sys::running_ext.pop_front();
                }
            },
            DS_cores);

#ifndef NKR_NO_GRPC
        NekoGui_traffic::trafficLooper->loop_enabled = false;
        NekoGui_traffic::trafficLooper->loop_mutex.lock();
        if (NekoGui::dataStore->traffic_loop_interval != 0) {
            NekoGui_traffic::trafficLooper->UpdateAll();
            for (const auto &item: NekoGui_traffic::trafficLooper->items) {
                NekoGui::profileManager->GetProfile(item->id)->Save();
                runOnUiThread([=] { refresh_proxy_list(item->id); });
            }
        }
        NekoGui_traffic::trafficLooper->loop_mutex.unlock();

        if (!crash) {
            bool rpcOK;
            QString error = defaultClient->Stop(&rpcOK);
            if (rpcOK && !error.isEmpty()) {
                runOnUiThread([=] { MessageBoxWarning("Stop return error", error); });
                return false;
            } else if (!rpcOK) {
                return false;
            }
        }
#endif

        const auto clearedAuxProfiles = !sem && !crash && !NekoGui::dataStore->aux_profile_ports.isEmpty();
        if (clearedAuxProfiles) NekoGui::dataStore->aux_profile_ports.clear();
        NekoGui::dataStore->UpdateStartedId(-1919);
        if (clearedAuxProfiles) NekoGui::dataStore->Save();
        NekoGui::dataStore->need_keep_vpn_off = false;
        running_internal_tun = false;
        running = nullptr;

        runOnUiThread([=] {
            refresh_status();
            refresh_proxy_list(clearedAuxProfiles ? -1 : id);
        });

        return true;
    };

    if (!mu_stopping.tryLock()) {
        if (sem) sem_stopped.release();
        return;
    }

    // timeout message
    auto restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                                         QMessageBox::Yes | QMessageBox::No, this);
    connect(restartMsgbox, &QMessageBox::accepted, this, [=] { MW_dialog_message("", "RestartProgram"); });
    auto restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 5000);

    runOnNewThread([=] {
        // do stop
        MW_show_log(">>>>>>>> " + tr("Stopping profile %1").arg(running->bean->DisplayTypeAndName()));
        if (!neko_stop_stage2()) {
            MW_show_log("<<<<<<<< " + tr("Failed to stop, please restart the program."));
        }
        mu_stopping.unlock();
        if (sem) sem_stopped.release();
        // cancel timeout
        runOnUiThread([=] {
            restartMsgboxTimer->cancel();
            restartMsgboxTimer->deleteLater();
            restartMsgbox->deleteLater();
        });
    });
}

void MainWindow::CheckUpdate() {
    // on new thread...
#ifndef NKR_NO_GRPC
    bool ok;
    libcore::UpdateReq request;
    request.set_action(libcore::UpdateAction::Check);
    request.set_check_pre_release(NekoGui::dataStore->check_include_pre);
    auto response = NekoGui_rpc::defaultClient->Update(&ok, request);
    if (!ok) return;

    auto err = response.error();
    if (!err.empty()) {
        runOnUiThread([=] {
            MessageBoxWarning(QObject::tr("Update"), err.c_str());
        });
        return;
    }

    if (response.release_download_url() == nullptr) {
        runOnUiThread([=] {
            MessageBoxInfo(QObject::tr("Update"), QObject::tr("No update"));
        });
        return;
    }

    runOnUiThread([=] {
        auto allow_updater = !NekoGui::dataStore->flag_use_appdata;
        auto note_pre_release = response.is_pre_release() ? " (Pre-release)" : "";
        QMessageBox box(QMessageBox::Question, QObject::tr("Update") + note_pre_release,
                        QObject::tr("Update found: %1\nRelease note:\n%2").arg(response.assets_name().c_str(), response.release_note().c_str()));
        //
        QAbstractButton *btn1 = nullptr;
        if (allow_updater) {
            btn1 = box.addButton(QObject::tr("Update"), QMessageBox::AcceptRole);
        }
        QAbstractButton *btn2 = box.addButton(QObject::tr("Open in browser"), QMessageBox::AcceptRole);
        box.addButton(QObject::tr("Close"), QMessageBox::RejectRole);
        box.exec();
        //
        if (btn1 == box.clickedButton() && allow_updater) {
            // Download Update
            runOnNewThread([=] {
                bool ok2;
                libcore::UpdateReq request2;
                request2.set_action(libcore::UpdateAction::Download);
                auto response2 = NekoGui_rpc::defaultClient->Update(&ok2, request2);
                runOnUiThread([=] {
                    if (response2.error().empty()) {
                        auto q = QMessageBox::question(nullptr, QObject::tr("Update"),
                                                       QObject::tr("Update is ready, restart to install?"));
                        if (q == QMessageBox::StandardButton::Yes) {
                            this->exit_reason = 1;
                            on_menu_exit_triggered();
                        }
                    } else {
                        MessageBoxWarning(QObject::tr("Update"), response2.error().c_str());
                    }
                });
            });
        } else if (btn2 == box.clickedButton()) {
            QDesktopServices::openUrl(QUrl(response.release_url().c_str()));
        }
    });
#endif
}
