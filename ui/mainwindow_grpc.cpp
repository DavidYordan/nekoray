#include "./ui_mainwindow.h"
#include "mainwindow.h"

#include "db/Database.hpp"
#include "db/ConfigBuilder.hpp"
#include "db/traffic/TrafficLooper.hpp"
#include "main/ConfigMutation.hpp"
#include "main/ConfigTransaction.hpp"
#include "rpc/gRPC.h"
#include "sys/CoreProcess.hpp"
#include "ui/widget/MessageBoxTimer.h"

#include <QTimer>
#include <QThread>
#include <QInputDialog>
#include <QPushButton>
#include <QDesktopServices>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QSet>
#include <QCryptographicHash>
#include <QJsonDocument>

#include <algorithm>
#include <atomic>
#include <utility>

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

    QString configRecoveryBlockReason() {
        const auto runtimeReason = NekoGui_ConfigTransaction::RuntimeMutationBlockReason();
        if (!runtimeReason.isEmpty()) return runtimeReason;
        const auto issues = NekoGui_ConfigTransaction::BlockingTransactionIssues();
        return issues.isEmpty()
                   ? QString{}
                   : QStringLiteral("Configuration recovery is required: %1")
                         .arg(issues.join(QStringLiteral(" | ")));
    }
} // namespace

void MainWindow::setup_grpc() {
#ifndef NKR_NO_GRPC
    // Setup Connection
    defaultClient = new Client(
        [=](const QString& errStr) {
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
    QList<QThread*> speedtestingThreads;
    std::shared_ptr<std::atomic_bool> speedtestingCancelToken;

    QMutex runningLabelUrlTestMutex;
    bool runningLabelUrlTesting = false;
    quint64 runningLabelUrlTestGeneration = 0;

    bool beginSpeedtesting(std::shared_ptr<std::atomic_bool>* token) {
        QMutexLocker locker(&speedtestingMutex);
        if (speedtesting) return false;
        speedtesting = true;
        speedtestingThreads.clear();
        speedtestingCancelToken = std::make_shared<std::atomic_bool>(false);
        if (token != nullptr) *token = speedtestingCancelToken;
        return true;
    }

    bool cancelSpeedtesting(const std::shared_ptr<std::atomic_bool>& expectedToken = {}) {
        QList<QThread*> threadsToStop;
        {
            QMutexLocker locker(&speedtestingMutex);
            if (!speedtesting && speedtestingCancelToken == nullptr) return false;
            if (expectedToken != nullptr && speedtestingCancelToken != expectedToken) return false;
            if (speedtestingCancelToken != nullptr) speedtestingCancelToken->store(true);
            threadsToStop = speedtestingThreads;
            // Cancellation is only a request. A worker can still be blocked in
            // gRPC/core teardown, so keep the test marked active until every
            // worker reaches finishSpeedtesting(). Otherwise TUN could be
            // enabled while a temporary Box still owns live sockets.
        }
        for (auto* thread: threadsToStop) {
            if (thread == nullptr) continue;
            thread->requestInterruption();
            thread->quit();
            thread->exit();
        }
        return true;
    }

    bool finishSpeedtesting(const std::shared_ptr<std::atomic_bool>& token) {
        QMutexLocker locker(&speedtestingMutex);
        if (speedtestingCancelToken != token) return false;
        speedtesting = false;
        speedtestingThreads.clear();
        speedtestingCancelToken.reset();
        return true;
    }

    void registerSpeedtestingThread(QThread* thread) {
        QMutexLocker locker(&speedtestingMutex);
        if (thread != nullptr && !speedtestingThreads.contains(thread)) speedtestingThreads << thread;
    }

    void unregisterSpeedtestingThread(QThread* thread) {
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

    bool isRunningLabelUrlTest(quint64 generation) {
        QMutexLocker locker(&runningLabelUrlTestMutex);
        return runningLabelUrlTesting && runningLabelUrlTestGeneration == generation;
    }

#ifndef NKR_NO_GRPC
    QByteArray speedtestJsonFingerprint(const QJsonObject& object) {
        return QCryptographicHash::hash(
            QJsonDocument(object).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256);
    }

    QByteArray speedtestProfileFingerprint(
        const std::shared_ptr<NekoGui::ProxyEntity>& profile,
        const QJsonObject& beanSnapshot) {
        if (profile == nullptr) return {};
        return speedtestJsonFingerprint({
            {QStringLiteral("id"), profile->id},
            {QStringLiteral("gid"), profile->gid},
            {QStringLiteral("type"), profile->type},
            {QStringLiteral("bean"), beanSnapshot},
        });
    }

    struct ImmutableSpeedtestJob final {
        ImmutableSpeedtestJob(
            int profileId_,
            std::shared_ptr<NekoGui::ProxyEntity> profileIdentity_,
            QByteArray profileFingerprint_,
            QByteArray beanFingerprint_,
            QByteArray generatedConfigFingerprint_,
            libcore::TestReq request_)
            : profileId(profileId_),
              profileIdentity(std::move(profileIdentity_)),
              profileFingerprint(std::move(profileFingerprint_)),
              beanFingerprint(std::move(beanFingerprint_)),
              generatedConfigFingerprint(std::move(generatedConfigFingerprint_)),
              request(std::move(request_)) {}

        const int profileId;
        const std::shared_ptr<NekoGui::ProxyEntity> profileIdentity;
        const QByteArray profileFingerprint;
        const QByteArray beanFingerprint;
        const QByteArray generatedConfigFingerprint;
        const libcore::TestReq request;
    };

    void persistSpeedtestResult(
        MainWindow* window,
        const std::shared_ptr<NekoGui::ProxyEntity>& profile,
        bool updateLatency,
        int latency,
        const QString& report) {
        if (window == nullptr || profile == nullptr) return;

        const auto previousLatency = profile->latency;
        const auto previousReport = profile->full_test_report;
        if (updateLatency) profile->latency = latency;
        profile->full_test_report = report;
        profile->Save();

        if (!profile->last_save_succeeded) {
            if (!profile->last_save_indeterminate) {
                profile->latency = previousLatency;
                profile->full_test_report = previousReport;
                MW_show_log(QObject::tr(
                                "Speedtest result for profile %1 was not saved; the in-memory result was rolled back.")
                                .arg(profile->id));
            } else {
                MW_show_log(QObject::tr(
                                "Speedtest result save for profile %1 is indeterminate; recovery is required and the intended in-memory result was retained.")
                                .arg(profile->id));
            }
        }
        window->refresh_proxy_list(profile->id);
    }

    std::shared_ptr<NekoGui::ProxyEntity> currentProfileForSpeedtestJob(
        const std::shared_ptr<const ImmutableSpeedtestJob>& job,
        QString* mismatchReason) {
        if (mismatchReason != nullptr) mismatchReason->clear();
        const auto mismatch = [&](const QString& reason) {
            if (mismatchReason != nullptr) *mismatchReason = reason;
            return std::shared_ptr<NekoGui::ProxyEntity>{};
        };

        if (job == nullptr) return mismatch(QStringLiteral("missing job"));
        const auto current = NekoGui::profileManager->GetProfile(job->profileId);
        if (current == nullptr || current != job->profileIdentity || current->save_control_no_save) {
            return mismatch(QStringLiteral("profile was removed or replaced"));
        }
        if (current->bean == nullptr) return mismatch(QStringLiteral("profile bean is unavailable"));

        const auto beanSnapshot = current->bean->ToJson();
        if (speedtestJsonFingerprint(beanSnapshot) != job->beanFingerprint) {
            return mismatch(QStringLiteral("profile bean changed"));
        }
        if (speedtestProfileFingerprint(current, beanSnapshot) != job->profileFingerprint) {
            return mismatch(QStringLiteral("profile configuration changed"));
        }

        const auto currentConfig = NekoGui::BuildConfig(current, true, false);
        if (currentConfig == nullptr || !currentConfig->error.isEmpty()) {
            return mismatch(QStringLiteral("effective test configuration is no longer valid"));
        }
        if (speedtestJsonFingerprint(currentConfig->coreConfig) != job->generatedConfigFingerprint) {
            return mismatch(QStringLiteral("effective test configuration changed"));
        }
        return current;
    }

    void applySpeedtestResponse(
        MainWindow* window,
        const std::shared_ptr<const ImmutableSpeedtestJob>& job,
        bool rpcOK,
        const libcore::TestResp& response) {
        QString mismatchReason;
        const auto profile = currentProfileForSpeedtestJob(job, &mismatchReason);
        if (profile == nullptr) {
            MW_show_log(QObject::tr("Discarded stale speedtest result for profile %1: %2.")
                            .arg(job == nullptr ? -1 : job->profileId)
                            .arg(mismatchReason));
            return;
        }

        if (!rpcOK) {
            persistSpeedtestResult(
                window,
                profile,
                true,
                -1,
                QObject::tr("gRPC test failed."));
            return;
        }

        const auto responseError = QString::fromStdString(response.error());
        auto latency = responseError.isEmpty() ? response.ms() : -1;
        if (latency == 0) latency = 1; // nekoray uses zero to represent "not tested"
        persistSpeedtestResult(
            window,
            profile,
            true,
            latency,
            QString::fromStdString(response.full_report()));

        if (!responseError.isEmpty()) {
            MW_show_log(QObject::tr("[%1] test error: %2")
                            .arg(profile->bean->DisplayTypeAndName(), responseError));
        }
    }
#endif
} // namespace

bool MainWindow::hasActiveIsolatedTest() {
    {
        QMutexLocker locker(&speedtestingMutex);
        if (speedtesting) return true;
    }
    QMutexLocker locker(&runningLabelUrlTestMutex);
    return runningLabelUrlTesting;
}

void MainWindow::speedtest_current_group(int mode, bool test_group) {
    // menu_stop_testing
    if (mode == 114514) {
        if (cancelSpeedtesting()) {
            MW_show_log(QObject::tr("Speedtest stop requested."));
        }
        return;
    }

    if (NekoGui::dataStore->core_transition_depth.load() > 0) {
        MessageBoxWarning(software_name, tr("Wait for the current core transition to finish before starting an isolated test."));
        return;
    }

    auto profiles = get_selected_or_group();
    if (test_group) profiles = NekoGui::profileManager->CurrentGroup()->ProfilesWithOrder();
    if (profiles.isEmpty()) return;
    auto group = NekoGui::profileManager->CurrentGroup();
    if (group->archive) return;
    if (NekoGui::dataStore->spmode_vpn || isInternalTunActive()) {
        MessageBoxWarning(
            software_name,
            tr("Isolated profile tests are disabled while this project's Tun is requested or its worker is active because the temporary test socket could be captured and measured through the wrong line."));
        return;
    }

#ifndef NKR_NO_GRPC
    if (mode == libcore::TcpPing) {
        MessageBoxWarning(
            software_name,
            tr("TCP Ping is disabled because it uses a direct system socket and does not test the selected proxy line. Use URL Test instead."));
        return;
    }
#endif

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
    // Full Test options run a nested dialog. Re-check after it closes so a
    // core/TUN transition cannot slip between the first guard and test creation.
    if (NekoGui::dataStore->core_transition_depth.load() > 0) {
        MessageBoxWarning(software_name, tr("Wait for the current core transition to finish before starting an isolated test."));
        return;
    }
    if (NekoGui::dataStore->spmode_vpn || isInternalTunActive()) {
        MessageBoxWarning(
            software_name,
            tr("Isolated profile tests are disabled while this project's Tun is requested or its worker is active."));
        return;
    }
    if (!beginSpeedtesting(&cancelToken)) {
        MessageBoxWarning(software_name, QObject::tr("The last speed test did not exit completely, please wait. If it persists, use Stop Testing."));
        return;
    }

    const int configuredConcurrency = std::max(1, NekoGui::dataStore->test_concurrent);
    const auto latencyUrl = NekoGui::dataStore->test_latency_url.toStdString();
    const auto downloadUrl = NekoGui::dataStore->test_download_url.toStdString();
    const int downloadTimeout = NekoGui::dataStore->test_download_timeout;
    const bool fullLatency = full_test_flags.contains(QStringLiteral("1"));
    const bool fullUdpLatency = full_test_flags.contains(QStringLiteral("2"));
    const bool fullSpeed = full_test_flags.contains(QStringLiteral("3"));
    const bool fullInOut = full_test_flags.contains(QStringLiteral("4"));

    QList<std::shared_ptr<const ImmutableSpeedtestJob>> jobs;
    QSet<int> queuedProfileIds;
    for (const auto& profile: profiles) {
        if (profile == nullptr || profile->id < 0 || queuedProfileIds.contains(profile->id)) continue;

        const auto current = NekoGui::profileManager->GetProfile(profile->id);
        if (current == nullptr || current != profile || current->save_control_no_save) {
            MW_show_log(tr("Skipped stale speedtest selection for profile %1.").arg(profile->id));
            continue;
        }
        queuedProfileIds.insert(profile->id);
        if (current->bean == nullptr) {
            MW_show_log(tr("Skipped speedtest for profile %1 because its bean is unavailable.").arg(current->id));
            continue;
        }

        const auto testConfig = BuildConfig(current, true, false);
        if (testConfig == nullptr || !testConfig->error.isEmpty()) {
            persistSpeedtestResult(
                this,
                current,
                false,
                0,
                testConfig == nullptr ? tr("Unknown test config error.") : testConfig->error);
            continue;
        }

        libcore::TestReq request;
        request.set_mode(static_cast<libcore::TestMode>(mode));
        request.set_timeout(10 * 1000);
        request.set_url(latencyUrl);
        auto requestConfig = new libcore::LoadConfigReq;
        requestConfig->set_core_config(QJsonObject2QString(testConfig->coreConfig, false).toStdString());
        request.set_allocated_config(requestConfig);
        request.set_in_address(current->bean->serverAddress.toStdString());
        request.set_full_latency(fullLatency);
        request.set_full_udp_latency(fullUdpLatency);
        request.set_full_speed(fullSpeed);
        request.set_full_in_out(fullInOut);
        request.set_full_speed_url(downloadUrl);
        request.set_full_speed_timeout(downloadTimeout);

        const auto beanSnapshot = current->bean->ToJson();
        jobs.append(std::make_shared<const ImmutableSpeedtestJob>(
            current->id,
            current,
            speedtestProfileFingerprint(current, beanSnapshot),
            speedtestJsonFingerprint(beanSnapshot),
            speedtestJsonFingerprint(testConfig->coreConfig),
            std::move(request)));
    }

    if (jobs.isEmpty()) {
        if (finishSpeedtesting(cancelToken)) {
            MW_show_log(tr("Speedtest finished with no runnable profiles."));
        }
        return;
    }

    const int profileCount = static_cast<int>(jobs.size());
    const int batches = std::max(1, (profileCount + configuredConcurrency - 1) / configuredConcurrency);
    const int perBatchTimeoutSeconds = mode == libcore::FullTest
                                           ? std::max(20, downloadTimeout + 15)
                                           : 20;
    const int watchdogMs = std::clamp(batches * perBatchTimeoutSeconds * 1000, 30000, 10 * 60 * 1000);
    const int threadN = std::min(configuredConcurrency, profileCount);
    setTimeout([cancelToken] {
        if (cancelSpeedtesting(cancelToken)) {
            MW_show_log(QObject::tr("Speedtest timed out; cancellation was requested. Tun remains blocked until every test worker exits."));
        }
    },
               this, watchdogMs);

    runOnNewThread([this, jobs, threadN, cancelToken]() {
        QMutex queueMutex;
        QSemaphore doneSem;
        auto pendingJobs = jobs;

        // Threads
        for (int i = 0; i < threadN; i++) {
            runOnNewThread([&, cancelToken] {
                auto* thread = QThread::currentThread();
                registerSpeedtestingThread(thread);

                forever {
                    if (cancelToken->load()) break;

                    std::shared_ptr<const ImmutableSpeedtestJob> job;
                    {
                        QMutexLocker locker(&queueMutex);
                        if (!pendingJobs.isEmpty() && !cancelToken->load()) {
                            job = pendingJobs.takeFirst();
                        }
                    }
                    if (job == nullptr) break;

                    bool rpcOK = false;
                    auto response = defaultClient->Test(&rpcOK, job->request);
                    if (cancelToken->load()) continue;

                    std::shared_ptr<const libcore::TestResp> responseSnapshot =
                        std::make_shared<libcore::TestResp>(std::move(response));
                    QMetaObject::invokeMethod(
                        this,
                        [this, job, rpcOK, responseSnapshot, cancelToken] {
                            if (cancelToken->load()) return;
                            applySpeedtestResponse(this, job, rpcOK, *responseSnapshot);
                        },
                        Qt::BlockingQueuedConnection);
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
    if (NekoGui::dataStore->core_transition_depth.load() > 0) {
        MessageBoxWarning(software_name, tr("Wait for the current core transition to finish before starting an isolated test."));
        return;
    }
    if (NekoGui::dataStore->spmode_vpn || isInternalTunActive()) {
        MessageBoxWarning(
            software_name,
            tr("URL Test is disabled while this project's Tun is requested or its worker is active because an isolated test could be captured and measured through the wrong line."));
        return;
    }
    if (running == nullptr) {
        MessageBoxWarning(software_name, tr("No running profile to test."));
        return;
    }
    const auto testConfig = BuildConfig(running, true, false);
    if (testConfig == nullptr || !testConfig->error.isEmpty()) {
        MessageBoxWarning(
            "BuildConfig return error",
            testConfig == nullptr ? tr("Unknown test config error.") : testConfig->error);
        return;
    }
    const auto testUrl = NekoGui::dataStore->test_latency_url.toStdString();
    const auto generation = beginRunningLabelUrlTest();
    if (generation == 0) {
        MW_show_log(tr("UrlTest is already running."));
        return;
    }
    last_test_time = QTime::currentTime();
    ui->label_running->setText(tr("Testing"));
    setTimeout([this, generation] {
        if (!isRunningLabelUrlTest(generation)) return;
        last_test_time = QTime::currentTime();
        ui->label_running->setText(tr("Test Result") + ": " + tr("Unavailable"));
        MW_show_log(tr("UrlTest timed out; Tun remains blocked until the test RPC exits."));
    },
               this, 15000);

    runOnNewThread([=] {
        libcore::TestReq req;
        req.set_mode(libcore::UrlTest);
        req.set_timeout(10 * 1000);
        req.set_url(testUrl);
        auto config = new libcore::LoadConfigReq;
        config->set_core_config(QJsonObject2QString(testConfig->coreConfig, false).toStdString());
        req.set_allocated_config(config);

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

void MainWindow::finish_runtime_transition(const NekoGui_Runtime::TransitionTicket& transition) {
    NekoGui_Runtime::TransitionCompletion completion;
    std::set<std::uint64_t> crashGenerations;
    {
        // Keep the pending-generation set and the coordinator's pending bit in
        // one lock order. This closes the old Complete -> queued callback gap.
        std::lock_guard<std::mutex> pendingLock(pending_core_crash_mutex);
        completion = runtime_transition.CompleteOrHandoff(
            transition,
            &NekoGui::dataStore->core_transition_depth);
        if (!completion.completed) return;
        if (completion.handoff.valid) {
            crashGenerations.swap(pending_core_crash_generations);
        }
    }

    if (completion.handoff.valid) {
        dispatch_core_crash_cleanup(completion.handoff, std::move(crashGenerations));
    } else {
        dispatch_pending_daemon_profile_start();
    }
}

void MainWindow::queue_core_crash_cleanup(std::uint64_t daemonGeneration) {
    if (daemonGeneration == 0) return;

    NekoGui_Runtime::TransitionTicket transition;
    std::set<std::uint64_t> crashGenerations;
    {
        std::lock_guard<std::mutex> pendingLock(pending_core_crash_mutex);
        pending_core_crash_generations.insert(daemonGeneration);
        runtime_transition.RequestCrashCleanup(&NekoGui::dataStore->core_transition_depth);
        transition = runtime_transition.TryBegin(
            NekoGui_Runtime::TransitionKind::CrashCleanup,
            &NekoGui::dataStore->core_transition_depth);
        if (transition.valid) crashGenerations.swap(pending_core_crash_generations);
    }

    if (!transition.valid) {
        show_log_impl(tr("Core crash cleanup is queued until the current transition drains."));
        return;
    }
    dispatch_core_crash_cleanup(transition, std::move(crashGenerations));
}

void MainWindow::dispatch_core_crash_cleanup(
    const NekoGui_Runtime::TransitionTicket& transition,
    std::set<std::uint64_t> daemonGenerations) {
    QMetaObject::invokeMethod(
        this,
        [this, transition, daemonGenerations = std::move(daemonGenerations)] {
            if (!runtime_transition.IsCurrent(transition)) return;
            const auto crashedRuntimeGeneration = running_daemon_generation;
            if (crashedRuntimeGeneration == 0 ||
                daemonGenerations.find(crashedRuntimeGeneration) == daemonGenerations.end()) {
                MW_show_log(tr("Ignored stale core-crash cleanup because it does not own the observed runtime generation."));
                finish_runtime_transition(transition);
                return;
            }
            neko_stop(
                true,
                false,
                CoreStopReason::CoreCrashCleanup,
                false,
                {},
                transition,
                crashedRuntimeGeneration);
        },
        Qt::QueuedConnection);
}

void MainWindow::queue_daemon_profile_start(
    const NekoGui_Runtime::DaemonProfileStartRequest& request) {
    if (!request.valid) return;
    {
        std::lock_guard<std::mutex> pendingLock(pending_profile_start_mutex);
        pending_profile_start = request;
    }
    dispatch_pending_daemon_profile_start();
}

void MainWindow::dispatch_pending_daemon_profile_start() {
    NekoGui_Runtime::DaemonProfileStartRequest request;
    {
        std::lock_guard<std::mutex> pendingLock(pending_profile_start_mutex);
        if (!pending_profile_start.has_value() || pending_profile_start_dispatch_queued) return;
        request = *pending_profile_start;
        pending_profile_start_dispatch_queued = true;
    }

    QMetaObject::invokeMethod(
        this,
        [this, request] {
            bool requestStillCurrent = false;
            {
                std::lock_guard<std::mutex> pendingLock(pending_profile_start_mutex);
                pending_profile_start_dispatch_queued = false;
                requestStillCurrent = pending_profile_start.has_value() &&
                                      pending_profile_start->daemonGeneration == request.daemonGeneration &&
                                      pending_profile_start->requestGeneration == request.requestGeneration &&
                                      pending_profile_start->profileId == request.profileId;
            }
            if (!requestStillCurrent) {
                // A newer event may have replaced this one while its callback
                // was queued. Ensure the replacement receives its own turn.
                dispatch_pending_daemon_profile_start();
                return;
            }
            neko_start(
                request.profileId,
                CoreStartReason::CoreCrashRecovery,
                request.daemonGeneration,
                request.requestGeneration);
        },
        Qt::QueuedConnection);
}

void MainWindow::clear_pending_daemon_profile_start(
    const NekoGui_Runtime::DaemonProfileStartRequest& request) {
    std::lock_guard<std::mutex> pendingLock(pending_profile_start_mutex);
    if (!pending_profile_start.has_value() ||
        pending_profile_start->daemonGeneration != request.daemonGeneration ||
        pending_profile_start->requestGeneration != request.requestGeneration ||
        pending_profile_start->profileId != request.profileId) {
        return;
    }
    pending_profile_start.reset();
}

void MainWindow::neko_start(
    int _id,
    CoreStartReason reason,
    std::uint64_t expectedDaemonGeneration,
    std::uint64_t expectedRequestGeneration) {
    if (NekoGui::dataStore->prepare_exit) return;

    auto ents = get_now_selected_list();
    auto ent = (_id < 0 && !ents.isEmpty()) ? ents.first() : NekoGui::profileManager->GetProfile(_id);
    const NekoGui_Runtime::DaemonProfileStartRequest readyRequest{
        expectedDaemonGeneration,
        expectedRequestGeneration,
        _id,
        expectedDaemonGeneration != 0 && expectedRequestGeneration != 0 && _id >= 0,
    };
    if (ent == nullptr) {
        if (readyRequest.valid) {
            (void) core_process->ConsumeQueuedProfileStart(
                readyRequest.daemonGeneration,
                readyRequest.requestGeneration,
                readyRequest.profileId);
            clear_pending_daemon_profile_start(readyRequest);
        }
        return;
    }

    if (select_mode && !readyRequest.valid) {
        emit profile_selected(ent->id);
        select_mode = false;
        refresh_status();
        return;
    }

    const auto transition = runtime_transition.TryBegin(
        NekoGui_Runtime::TransitionKind::Start,
        &NekoGui::dataStore->core_transition_depth);
    if (!transition.valid) {
        if (readyRequest.valid) {
            MW_show_log(tr("Queued profile start is waiting for the current core transition to finish."));
        } else {
            MessageBoxWarning(software_name, tr("Wait for the current core transition to finish."));
        }
        return;
    }
    const auto finishTransition = [this, transition] {
        finish_runtime_transition(transition);
    };

    if (readyRequest.valid) {
        const auto consumed = core_process->ConsumeQueuedProfileStart(
            readyRequest.daemonGeneration,
            readyRequest.requestGeneration,
            readyRequest.profileId);
        clear_pending_daemon_profile_start(readyRequest);
        if (!consumed) {
            MW_show_log(tr("Discarded a stale or explicitly cancelled queued core-start event."));
            finishTransition();
            return;
        }
    } else {
        // A direct user/reload Start supersedes every older daemon-ready
        // request. Clear both CoreProcess's emitted capability and the local
        // delivery record before this new candidate is built.
        (void) core_process->CancelQueuedProfileStart();
        std::lock_guard<std::mutex> pendingLock(pending_profile_start_mutex);
        pending_profile_start.reset();
    }

    // Acquire before reading the live model. A mutation that passed its depth
    // check just before this Start set the gate may still be in progress.
    NekoGui_ConfigMutation::Guard candidateMutationGuard(false);
    if (!candidateMutationGuard.acquired()) {
        MW_show_log("<<<<<<<< " + tr("Profile start/reload could not snapshot the configuration because another model mutation is in progress."));
        finishTransition();
        return;
    }

    if (expectedDaemonGeneration != 0 &&
        !core_process->IsDaemonReady(expectedDaemonGeneration)) {
        MW_show_log(tr("Discarded a stale queued profile start for daemon generation %1.")
                        .arg(expectedDaemonGeneration));
        finishTransition();
        return;
    }

    const auto recoveryBlock = configRecoveryBlockReason();
    if (!recoveryBlock.isEmpty()) {
        MessageBoxWarning(
            software_name,
            tr("Profile start/reload was blocked until configuration recovery is completed.\n%1")
                .arg(recoveryBlock));
        finishTransition();
        return;
    }

    auto group = NekoGui::profileManager->GetGroup(ent->gid);
    if (group == nullptr || group->archive) {
        finishTransition();
        return;
    }
    if (group->front_proxy_id >= 0) {
        auto frontProxy = NekoGui::profileManager->GetProfile(group->front_proxy_id);
        show_log_impl(tr("Group front proxy active: %1")
                          .arg(frontProxy == nullptr || frontProxy->bean == nullptr
                                   ? QStringLiteral("#%1").arg(group->front_proxy_id)
                                   : frontProxy->bean->DisplayTypeAndName()));
    }

    if (internalTunWouldBeInterrupted() &&
        reason != CoreStartReason::EnableInternalTun &&
        reason != CoreStartReason::DisableInternalTun &&
        reason != CoreStartReason::CoreCrashRecovery) {
        MessageBoxWarning(software_name, internalTunReloadBlockedMessage());
        finishTransition();
        return;
    }
    if (tunModeChangePendingWhileRunning() &&
        reason != CoreStartReason::EnableInternalTun &&
        reason != CoreStartReason::DisableInternalTun &&
        reason != CoreStartReason::CoreCrashRecovery) {
        MessageBoxWarning(software_name, tunModeChangeBlockedMessage());
        finishTransition();
        return;
    }

    auto result = BuildConfig(ent, false, false);
    if (!result->error.isEmpty()) {
        MessageBoxWarning("BuildConfig return error", result->error);
        finishTransition();
        return;
    }

    const auto candidateProfileId = ent->id;
    const auto candidateCoreConfigBytes = QJsonObject2QString(result->coreConfig, false).toUtf8();
    const auto candidateCoreConfig = candidateCoreConfigBytes.toStdString();
    const auto candidateConfigSha256 =
        QCryptographicHash::hash(candidateCoreConfigBytes, QCryptographicHash::Sha256).toHex();
    const auto candidateInternalTun = result->managedInternalTun;
    const auto candidateConnectionStatistics = NekoGui::dataStore->connection_statistics;
    const auto candidateTrafficLoopInterval = NekoGui::dataStore->traffic_loop_interval;
    const auto candidateDaemonGeneration = core_process->CurrentDaemonGeneration();

    if (expectedDaemonGeneration != 0 &&
        candidateDaemonGeneration != expectedDaemonGeneration) {
        MW_show_log(tr("Discarded a queued profile start after the core daemon generation changed."));
        finishTransition();
        return;
    }

    if (!core_process->IsDaemonReady(candidateDaemonGeneration)) {
        if (expectedDaemonGeneration != 0) {
            MW_show_log(tr("Discarded a queued profile start because its core daemon is no longer ready."));
            finishTransition();
            return;
        }
        const auto immediatelyReadyRequest =
            core_process->QueueProfileStartWhenCoreIsUp(candidateProfileId);
        if (immediatelyReadyRequest.valid) {
            queue_daemon_profile_start(immediatelyReadyRequest);
        } else {
            runOnUiThread(
                [=] {
                    MW_show_log("Try to start the config, but the core has not listened to the grpc port, so restart it...");
                    core_process->EnsureStarted();
                },
                DS_cores);
        }
        finishTransition();
        return; // a generation-bound CoreStarted event will resume this request
    }

    auto neko_start_stage2 = [=] {
        struct StartRpcResult {
            bool requestSent = false;
            bool confirmed = false;
            QString indeterminateReason;
        };

        if (!runtime_transition.IsCurrent(transition)) {
            MW_show_log("<<<<<<<< " + tr("Ignoring a stale profile-start generation."));
            return false;
        }
        // Keep the immutable candidate fenced against model/disk mutation
        // through the Start RPC. Runtime bookkeeping is committed on the UI
        // thread after these guards are released; otherwise UpdateStartedId's
        // own durable Save would deadlock on a mutex owned by this worker.
        const auto startRpc = [&]() -> StartRpcResult {
            NekoGui_ConfigMutation::Guard stageMutationGuard(true);
            if (!stageMutationGuard.acquired()) {
                MW_show_log("<<<<<<<< " + tr("Profile start/reload could not acquire the configuration model lock."));
                return {};
            }
            NekoGui_ConfigTransaction::DiskLockGuard stageDiskLock;
            if (!stageDiskLock.acquired()) {
                MW_show_log("<<<<<<<< " + tr("Profile start/reload could not acquire the configuration disk lock: %1")
                                              .arg(stageDiskLock.error()));
                return {};
            }
            const auto stageRecoveryBlock = configRecoveryBlockReason();
            if (!stageRecoveryBlock.isEmpty()) {
                MW_show_log("<<<<<<<< " + tr("Profile start/reload blocked by configuration recovery: %1")
                                              .arg(stageRecoveryBlock));
                return {};
            }
            if (!runtime_transition.IsCurrent(transition)) return {};
            if (!core_process->IsDaemonReady(candidateDaemonGeneration)) return {};
#ifndef NKR_NO_GRPC
            libcore::LoadConfigReq req;
            req.set_core_config(candidateCoreConfig);
            req.set_enable_nekoray_connections(candidateConnectionStatistics);
            if (candidateTrafficLoopInterval > 0) {
                QSet<QString> statsTags;
                for (const auto& item: result->outboundStats) {
                    const auto tag = QString::fromStdString(item.outboundTag);
                    if (!tag.isEmpty()) statsTags.insert(tag);
                }
                statsTags.insert("proxy");
                for (const auto& tag: statsTags) {
                    req.add_stats_outbounds(tag.toStdString());
                }
                req.add_stats_outbounds("bypass");
            }
            bool rpcOK;
            const auto error = defaultClient->Start(&rpcOK, req);
            if (rpcOK && !error.isEmpty()) {
                runOnUiThread([=] { MessageBoxWarning("LoadConfig return error", error); });
                return {
                    true,
                    false,
                    tr("The Start RPC returned an application error after the candidate may have acquired resources: %1")
                        .arg(error),
                };
            } else if (!rpcOK) {
                return {
                    true,
                    false,
                    tr("The Start RPC transport failed after request delivery became indeterminate."),
                };
            }
            if (!core_process->IsDaemonReady(candidateDaemonGeneration)) {
                return {
                    true,
                    false,
                    tr("The core daemon changed or stopped before the successful Start response could be committed."),
                };
            }
#endif
            return {true, true, {}};
        };
        const auto startRpcResult = startRpc();
        if (!startRpcResult.requestSent) return false;
        if (!startRpcResult.confirmed) {
            bool indeterminateStateRecorded = false;
            QMetaObject::invokeMethod(
                this,
                [=, &indeterminateStateRecorded] {
                    if (!runtime_transition.IsCurrent(transition)) return;
                    const auto conservativeDaemonGeneration = std::max(
                        candidateDaemonGeneration,
                        core_process->CurrentDaemonGeneration());
                    NekoGui::dataStore->ignoreConnTag = result->ignoreConnTag;
                    NekoGui::dataStore->UpdateStartedId(candidateProfileId);
                    // A candidate that requested Tun is conservatively treated
                    // as active until a sequenced, confirmed Stop or a crash of
                    // its possible daemon generation proves otherwise.
                    running_internal_tun = running_internal_tun || candidateInternalTun;
                    running_generation = transition.generation;
                    // The RPC protocol is not generation-bound yet. Attribute
                    // uncertainty to the newest local daemon observed by this
                    // commit. This can retain state longer than necessary, but
                    // cannot let an older crash clear a request that may have
                    // crossed a daemon restart on the reused channel.
                    running_daemon_generation = conservativeDaemonGeneration;
                    running_config_sha256 = candidateConfigSha256;
                    running = ent;
                    runtime_state_indeterminate = true;
                    runtime_state_indeterminate_reason = startRpcResult.indeterminateReason;
                    refresh_status();
                    refresh_proxy_list(candidateProfileId);
                    indeterminateStateRecorded = true;
                },
                Qt::BlockingQueuedConnection);
            if (indeterminateStateRecorded) {
                MW_show_log("<<<<<<<< " + tr("Runtime state is indeterminate and remains fail-closed until Tun is explicitly disabled or another permitted Stop succeeds: %1")
                                                .arg(startRpcResult.indeterminateReason));
            }
            return false;
        }

        if (!runtime_transition.IsCurrent(transition) ||
            !core_process->IsDaemonReady(candidateDaemonGeneration)) {
            // The request was acknowledged but ownership changed before the UI
            // commit. Preserve a fail-closed observation instead of silently
            // treating the candidate as stopped.
            QMetaObject::invokeMethod(
                this,
                [=] {
                    if (!runtime_transition.IsCurrent(transition)) return;
                    const auto conservativeDaemonGeneration = std::max(
                        candidateDaemonGeneration,
                        core_process->CurrentDaemonGeneration());
                    NekoGui::dataStore->UpdateStartedId(candidateProfileId);
                    running_internal_tun = running_internal_tun || candidateInternalTun;
                    running_generation = transition.generation;
                    running_daemon_generation = conservativeDaemonGeneration;
                    running_config_sha256 = candidateConfigSha256;
                    running = ent;
                    runtime_state_indeterminate = true;
                    runtime_state_indeterminate_reason = tr(
                        "The daemon generation changed between Start acknowledgement and runtime commit.");
                    refresh_status();
                    refresh_proxy_list(candidateProfileId);
                },
                Qt::BlockingQueuedConnection);
            return false;
        }

        bool stateCommitted = false;
        bool stateIndeterminate = false;
        QMetaObject::invokeMethod(
            this,
            [=, &stateCommitted, &stateIndeterminate] {
                if (!runtime_transition.IsCurrent(transition)) return;
                if (!core_process->IsDaemonReady(candidateDaemonGeneration)) {
                    // The daemon can stop after the worker's acknowledgement
                    // check but before this UI-thread commit. Retain the
                    // candidate observation under the newest local generation
                    // seen at commit. This conservative upper bound prevents
                    // an older crash from clearing a possibly cross-restart
                    // request; it does not identify the actual RPC receiver.
                    const auto conservativeDaemonGeneration = std::max(
                        candidateDaemonGeneration,
                        core_process->CurrentDaemonGeneration());
                    NekoGui::dataStore->UpdateStartedId(candidateProfileId);
                    running_internal_tun = running_internal_tun || candidateInternalTun;
                    running_generation = transition.generation;
                    running_daemon_generation = conservativeDaemonGeneration;
                    running_config_sha256 = candidateConfigSha256;
                    running = ent;
                    runtime_state_indeterminate = true;
                    runtime_state_indeterminate_reason = tr(
                        "The core daemon stopped after Start acknowledgement but before runtime state commit.");
                    refresh_status();
                    refresh_proxy_list(candidateProfileId);
                    stateIndeterminate = true;
                    return;
                }
                NekoGui::dataStore->ignoreConnTag = result->ignoreConnTag;
                NekoGui::dataStore->UpdateStartedId(candidateProfileId);
                running_internal_tun = candidateInternalTun;
                running_generation = transition.generation;
                running_daemon_generation = candidateDaemonGeneration;
                running_config_sha256 = candidateConfigSha256;
                running = ent;
                runtime_state_indeterminate = false;
                runtime_state_indeterminate_reason.clear();
                refresh_status();
                refresh_proxy_list(candidateProfileId);
                stateCommitted = true;
            },
            Qt::BlockingQueuedConnection);

#ifndef NKR_NO_GRPC
        if (stateCommitted) {
            QMutexLocker trafficLock(&NekoGui_traffic::trafficLooper->loop_mutex);
            NekoGui_traffic::trafficLooper->proxy = result->outboundStat;
            NekoGui_traffic::trafficLooper->items = result->outboundStats;
            NekoGui_traffic::trafficLooper->loop_enabled = true;
        }
#endif
        if (stateIndeterminate) {
            MW_show_log("<<<<<<<< " + tr(
                "Runtime state became indeterminate after Start acknowledgement; waiting for generation-bound cleanup."));
        }
        return stateCommitted;
    };

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
            const auto stopSucceeded = std::make_shared<std::atomic_bool>(false);
            runOnUiThread([=] {
                neko_stop(stopCrash, true, stopReason, true, stopSucceeded, transition);
            });
            sem_stopped.acquire();
            if (!stopSucceeded->load()) {
                MW_show_log("<<<<<<<< " + tr("Profile start/reload was aborted because the old generation did not stop cleanly."));
                finishTransition();
                runOnUiThread([=] {
                    restartMsgboxTimer->cancel();
                    restartMsgboxTimer->deleteLater();
                    restartMsgbox->deleteLater();
                });
                return;
            }
        }
        // do start
        MW_show_log(">>>>>>>> " + tr("Starting profile %1").arg(ent->bean->DisplayTypeAndName()));
        if (!neko_start_stage2()) {
            MW_show_log("<<<<<<<< " + tr("Failed to start profile %1").arg(ent->bean->DisplayTypeAndName()));
        }
        finishTransition();
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

void MainWindow::neko_stop(bool crash, bool sem, CoreStopReason reason,
                           bool nestedTransition,
                           const std::shared_ptr<std::atomic_bool>& completionResult,
                           const NekoGui_Runtime::TransitionTicket& ownerTransition,
                           std::uint64_t expectedCrashDaemonGeneration) {
    if (completionResult != nullptr) completionResult->store(false);
    // Explicit Stop revokes both queued and already-emitted daemon-ready
    // requests even when another transition currently owns the coordinator.
    if (reason == CoreStopReason::UserAction || reason == CoreStopReason::AppExit) {
        (void) core_process->CancelQueuedProfileStart();
    }

    NekoGui_Runtime::TransitionTicket transition;
    bool ownsTransition = false;
    if (ownerTransition.valid) {
        transition = ownerTransition;
        const auto validNestedOwner = nestedTransition &&
                                      transition.kind == NekoGui_Runtime::TransitionKind::Start;
        const auto validCrashCleanupOwner = !nestedTransition &&
                                            reason == CoreStopReason::CoreCrashCleanup &&
                                            transition.kind == NekoGui_Runtime::TransitionKind::CrashCleanup;
        if ((!validNestedOwner && !validCrashCleanupOwner) ||
            !runtime_transition.IsCurrent(transition)) {
            qCritical() << "Refusing Stop with an invalid transition owner.";
            if (sem) sem_stopped.release();
            return;
        }
        ownsTransition = validCrashCleanupOwner;
    } else if (!nestedTransition) {
        transition = runtime_transition.TryBegin(
            NekoGui_Runtime::TransitionKind::Stop,
            &NekoGui::dataStore->core_transition_depth);
        if (!transition.valid) {
            MessageBoxWarning(software_name, tr("Wait for the current core transition to finish."));
            if (sem) sem_stopped.release();
            return;
        }
        ownsTransition = true;
    } else {
        qCritical() << "Refusing a nested Stop without an owning Start transition.";
        if (sem) sem_stopped.release();
        return;
    }

    const auto finishTransition = [this, transition, ownsTransition] {
        if (ownsTransition) finish_runtime_transition(transition);
    };

    if (reason == CoreStopReason::CoreCrashCleanup &&
        (expectedCrashDaemonGeneration == 0 ||
         running_daemon_generation != expectedCrashDaemonGeneration)) {
        MW_show_log(tr("Ignored stale core-crash cleanup for daemon generation %1.")
                        .arg(expectedCrashDaemonGeneration));
        if (completionResult != nullptr) completionResult->store(true);
        finishTransition();
        if (sem) sem_stopped.release();
        return;
    }

    auto id = NekoGui::dataStore->started_id;
    if (id < 0) {
        if (reason == CoreStopReason::CoreCrashCleanup) {
            running_internal_tun = false;
            running_generation = 0;
            running_daemon_generation = 0;
            running_config_sha256.clear();
            running = nullptr;
            runtime_state_indeterminate = false;
            runtime_state_indeterminate_reason.clear();
            refresh_status();
        }
        if (completionResult != nullptr) completionResult->store(true);
        finishTransition();
        if (sem) sem_stopped.release();
        return;
    }

    if (!crash && internalTunWouldBeInterrupted() &&
        reason != CoreStopReason::EnableInternalTun &&
        reason != CoreStopReason::DisableInternalTun) {
        finishTransition();
        if (sem) sem_stopped.release();
        MessageBoxWarning(software_name, internalTunReloadBlockedMessage());
        return;
    }

    const auto runningName = running == nullptr || running->bean == nullptr
                                 ? QStringLiteral("#%1").arg(id)
                                 : running->bean->DisplayTypeAndName();
    const auto persistTrafficOnStop = NekoGui::dataStore->traffic_loop_interval != 0;

    auto neko_stop_stage2 = [=] {
        if (!runtime_transition.IsCurrent(transition)) return false;
#ifndef NKR_NO_GRPC
        bool trafficLoopWasEnabled = false;
        QSet<int> trafficProfileIds;
        {
            QMutexLocker trafficLock(&NekoGui_traffic::trafficLooper->loop_mutex);
            trafficLoopWasEnabled = NekoGui_traffic::trafficLooper->loop_enabled.load();
            NekoGui_traffic::trafficLooper->loop_enabled.store(false);
            if (persistTrafficOnStop && !crash) {
                NekoGui_traffic::trafficLooper->UpdateAll();
                for (const auto& item: NekoGui_traffic::trafficLooper->items) {
                    if (item.profileId >= 0) trafficProfileIds.insert(item.profileId);
                }
            }
        }

        if (!trafficProfileIds.isEmpty()) {
            QMetaObject::invokeMethod(
                this,
                [=] {
                    for (const auto profileId: trafficProfileIds) {
                        const auto profile = NekoGui::profileManager->GetProfile(profileId);
                        if (profile == nullptr) {
                            qWarning() << "Skipping traffic save for an explicitly deleted profile:" << profileId;
                            continue;
                        }
                        profile->Save();
                        refresh_proxy_list(profileId);
                    }
                },
                Qt::BlockingQueuedConnection);
        }

        const auto markStopIndeterminate = [=](const QString& detail) {
            QMetaObject::invokeMethod(
                this,
                [=] {
                    if (!runtime_transition.IsCurrent(transition)) return;
                    // Preserve the last observed running/Tun state. A lost
                    // response may mean Stop succeeded, while an application
                    // error may mean shutdown retained resources.
                    runtime_state_indeterminate = true;
                    runtime_state_indeterminate_reason = detail;
                    refresh_status();
                },
                Qt::BlockingQueuedConnection);
        };

        if (!crash) {
            bool rpcOK;
            QString error = defaultClient->Stop(&rpcOK);
            if (rpcOK && !error.isEmpty()) {
                {
                    QMutexLocker trafficLock(&NekoGui_traffic::trafficLooper->loop_mutex);
                    NekoGui_traffic::trafficLooper->loop_enabled.store(trafficLoopWasEnabled);
                }
                markStopIndeterminate(
                    tr("The Stop RPC returned an application error; retained runtime resources cannot be ruled out: %1")
                        .arg(error));
                runOnUiThread([=] { MessageBoxWarning("Stop return error", error); });
                return false;
            } else if (!rpcOK) {
                {
                    QMutexLocker trafficLock(&NekoGui_traffic::trafficLooper->loop_mutex);
                    NekoGui_traffic::trafficLooper->loop_enabled.store(trafficLoopWasEnabled);
                }
                markStopIndeterminate(
                    tr("The Stop RPC transport failed; whether the daemon stopped the runtime is unknown."));
                return false;
            }
        }
#endif

        // Auxiliary port bindings are persistent user configuration. Stopping,
        // restarting, crashing or exiting the current line must not silently
        // delete them; only an explicit mapping edit may change this state.
        QMetaObject::invokeMethod(
            this,
            [=] {
                NekoGui::dataStore->UpdateStartedId(-1919);
                NekoGui::dataStore->need_keep_vpn_off = false;
                running_internal_tun = false;
                running_generation = 0;
                running_daemon_generation = 0;
                running_config_sha256.clear();
                running = nullptr;
                runtime_state_indeterminate = false;
                runtime_state_indeterminate_reason.clear();
                refresh_status();
                refresh_proxy_list(id);
            },
            Qt::BlockingQueuedConnection);

        return true;
    };

    // timeout message
    auto restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                                         QMessageBox::Yes | QMessageBox::No, this);
    connect(restartMsgbox, &QMessageBox::accepted, this, [=] { MW_dialog_message("", "RestartProgram"); });
    auto restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 5000);

    runOnNewThread([=] {
        // do stop
        MW_show_log(">>>>>>>> " + tr("Stopping profile %1").arg(runningName));
        const auto stopped = neko_stop_stage2();
        if (!stopped) {
            MW_show_log("<<<<<<<< " + tr("Failed to stop, please restart the program."));
        }
        if (completionResult != nullptr) completionResult->store(stopped);
        finishTransition();
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
    runOnUiThread([=] {
        MessageBoxInfo(QObject::tr("Update"), QObject::tr("Online update is disabled in this private build."));
    });
}
