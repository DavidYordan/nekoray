#include "CoreProcess.hpp"
#include "main/NekoGui.hpp"

#include <QElapsedTimer>

namespace NekoGui_sys {

    QElapsedTimer coreRestartTimer;

    CoreProcess::CoreProcess(const QString &core_path, const QStringList &args) : QProcess() {
        program = core_path;
        arguments = args;
        env = QProcessEnvironment::systemEnvironment().toStringList();

        connect(this, &QProcess::readyReadStandardOutput, this, [&]() {
            auto log = readAllStandardOutput();
            if (!NekoGui::dataStore->core_running) {
                if (log.contains("grpc server listening")) {
                    // The core really started
                    NekoGui::dataStore->core_running = true;
                    const auto request = daemonGeneration.MarkProcessReady();
                    if (request.valid) {
                        MW_dialog_message(
                            "CoreProcess",
                            QStringLiteral("CoreStarted,%1,%2,%3")
                                .arg(request.daemonGeneration)
                                .arg(request.requestGeneration)
                                .arg(request.profileId));
                    }
                } else if (log.contains("failed to serve")) {
                    // The core failed to start
                    QProcess::kill();
                }
            }
            if (logCounter.fetchAndAddRelaxed(log.count("\n")) > NekoGui::dataStore->max_log_line) return;
            MW_show_log(log);
        });
        connect(this, &QProcess::readyReadStandardError, this, [&]() {
            auto log = readAllStandardError().trimmed();
            if (show_stderr) {
                MW_show_log(log);
                return;
            }
            if (log.contains("token is set")) {
                show_stderr = true;
            }
        });
        connect(this, &QProcess::errorOccurred, this, [&](QProcess::ProcessError error) {
            if (error == QProcess::ProcessError::FailedToStart) {
                failed_to_start = true;
                MW_show_log("start core error occurred: " + errorString() + "\n");
            }
        });
        connect(this, &QProcess::stateChanged, this, [&](QProcess::ProcessState state) {
            std::uint64_t stoppedGeneration = 0;
            if (state == QProcess::NotRunning) {
                NekoGui::dataStore->core_running = false;
                stoppedGeneration = daemonGeneration.MarkProcessStopped();
            }

            if (!NekoGui::dataStore->prepare_exit && state == QProcess::NotRunning) {
                if (failed_to_start) return; // no retry
                if (restarting) return;

                MW_dialog_message(
                    "CoreProcess",
                    QStringLiteral("CoreCrashed,%1").arg(stoppedGeneration));

                // Retry rate limit
                if (coreRestartTimer.isValid()) {
                    if (coreRestartTimer.restart() < 10 * 1000) {
                        coreRestartTimer = QElapsedTimer();
                        MW_show_log("[Error] " + QObject::tr("Core exits too frequently, stop automatic restart this profile."));
                        return;
                    }
                } else {
                    coreRestartTimer.start();
                }

                // Restarting the empty control daemon is not an OS-network
                // mode change.  Recreating a profile that requested Tun is:
                // after a crash it must therefore remain stopped until the
                // user explicitly starts it again.
                (void) daemonGeneration.CancelQueuedProfile();
                MW_show_log("[Error] " + QObject::tr(
                                "Core exited. Restarting the empty control core only; the profile, system proxy, and Tun will not be restored automatically."));
                const auto crashGeneration = daemonGeneration.CrashRestartToken();
                setTimeout(
                    [=] {
                        if (!daemonGeneration.CanRunCrashRestart(crashGeneration) ||
                            this->state() != QProcess::NotRunning || NekoGui::dataStore->prepare_exit) {
                            return;
                        }
                        Restart();
                    },
                    this,
                    1000);
            }
        });
    }

    void CoreProcess::Start() {
        if (started) return;
        started = true;
        failed_to_start = false;
        show_stderr = false;
        (void) daemonGeneration.BeginProcessStart();
        // cwd: same as GUI, at ./config
        QProcess::setEnvironment(env);
        QProcess::start(program, arguments);
        write((NekoGui::dataStore->core_token + "\n").toUtf8());
    }

    void CoreProcess::Restart() {
        restarting = true;
        QProcess::kill();
        QProcess::waitForFinished(500);
        started = false;
        Start();
        restarting = false;
    }

    void CoreProcess::EnsureStarted() {
        if (state() == QProcess::NotRunning) Restart();
    }

    NekoGui_Runtime::DaemonProfileStartRequest
    CoreProcess::QueueProfileStartWhenCoreIsUp(int profileId) {
        return daemonGeneration.QueueProfileForNextStart(profileId);
    }

    bool CoreProcess::CancelQueuedProfileStart() {
        return daemonGeneration.CancelQueuedProfile();
    }

    bool CoreProcess::ConsumeQueuedProfileStart(
        std::uint64_t daemonGenerationValue,
        std::uint64_t requestGeneration,
        int profileId) {
        return daemonGeneration.ConsumeReadyProfile({
            daemonGenerationValue,
            requestGeneration,
            profileId,
            true,
        });
    }

    std::uint64_t CoreProcess::CurrentDaemonGeneration() const {
        return daemonGeneration.CurrentGeneration();
    }

    bool CoreProcess::IsDaemonReady(std::uint64_t daemonGenerationValue) const {
        return daemonGeneration.IsReady(daemonGenerationValue);
    }

} // namespace NekoGui_sys
