#include "CoreProcess.hpp"
#include "main/NekoGui.hpp"

#include <QElapsedTimer>
#include <QUuid>

namespace NekoGui_sys {

    QElapsedTimer coreRestartTimer;

    CoreProcess::CoreProcess(const QString &core_path, const QStringList &args) : QProcess() {
        program = core_path;
        arguments = args;
        env = QProcessEnvironment::systemEnvironment().toStringList();

        connect(this, &QProcess::readyReadStandardOutput, this, [&]() {
            auto log = readAllStandardOutput();
            HandleCoreLog(stdoutProbeBuffer, log);
            if (logCounter.fetchAndAddRelaxed(log.count("\n")) > NekoGui::dataStore->max_log_line) return;
            MW_show_log(log);
        });
        connect(this, &QProcess::readyReadStandardError, this, [&]() {
            const auto rawLog = readAllStandardError();
            HandleCoreLog(stderrProbeBuffer, rawLog);
            auto log = rawLog.trimmed();
            if (show_stderr) {
                MW_show_log(log);
                return;
            }
            if (log.contains("token is set")) {
                show_stderr = true;
            }
        });
        connect(this, &QProcess::started, this, [&]() {
            const auto instance = daemonGeneration.CurrentInstance();
            const auto process = NekoGui_Runtime::DaemonProcessSnapshot{
                instance.generation,
                instance.instanceId,
                static_cast<std::int64_t>(processId()),
            };
            if (!daemonProcessExit.MarkProcessStarted(process)) {
                MW_show_log("[Error] " + QObject::tr(
                    "The core process started without an exclusive generation/UUID/PID finished fence."));
            }
            // Process creation is not RPC readiness. Proactively begin the
            // authenticated probe so readiness does not depend on a log line
            // arriving in one particular stream or read chunk.
            RequestDaemonHandshake();
        });
        connect(
            this,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [&](int exitCode, QProcess::ExitStatus exitStatus) {
                const auto finished = daemonProcessExit.MarkProcessFinished(
                    exitCode,
                    exitStatus == QProcess::NormalExit);
                if (!finished.valid) {
                    MW_show_log("[Error] " + QObject::tr(
                        "The core process finished without a generation/UUID/PID launch record."));
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
        stdoutProbeBuffer.clear();
        stderrProbeBuffer.clear();
        const auto instanceId = QUuid::createUuid()
                                    .toString(QUuid::WithoutBraces)
                                    .toStdString();
        (void) daemonGeneration.BeginProcessStart(instanceId);
        auto processArguments = arguments;
        processArguments.push_back(QStringLiteral("-instance-id"));
        processArguments.push_back(QString::fromStdString(instanceId));
        // cwd: same as GUI, at ./config
        QProcess::setEnvironment(env);
        QProcess::start(program, processArguments);
        write((NekoGui::dataStore->core_token + "\n").toUtf8());
    }

    void CoreProcess::Restart() {
        restarting = true;
        if (state() != QProcess::NotRunning) {
            // A running daemon may own network resources. Replacement is only
            // legal after its exact QProcess::finished signal; kill/terminate
            // is never an implicit restart mechanism.
            MW_show_log("[Error] " + QObject::tr(
                "The old core process has not confirmed exit; refusing to kill it or publish a replacement daemon identity."));
            restarting = false;
            return;
        }
        if (daemonProcessExit.CurrentProcess().valid()) {
            MW_show_log("[Error] " + QObject::tr(
                "The old core reached NotRunning without its exact finished signal; refusing to publish a replacement daemon identity."));
            restarting = false;
            return;
        }
        started = false;
        Start();
        restarting = false;
    }

    void CoreProcess::EnsureStarted() {
        if (state() == QProcess::NotRunning) {
            Restart();
            return;
        }
        const auto instance = daemonGeneration.CurrentInstance();
        if (instance.valid() && !instance.ready) {
            // A previous localhost handshake may have raced the server's
            // accept loop. An explicit profile start may safely request a new
            // handshake for the same empty control daemon.
            MW_dialog_message(
                "CoreProcess",
                QStringLiteral("CoreListening,%1,%2,0")
                    .arg(instance.generation)
                    .arg(QString::fromStdString(instance.instanceId)));
        }
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

    NekoGui_Runtime::DaemonInstanceSnapshot CoreProcess::CurrentDaemonInstance() const {
        return daemonGeneration.CurrentInstance();
    }

    NekoGui_Runtime::DaemonProcessSnapshot CoreProcess::CurrentDaemonProcess() const {
        return daemonProcessExit.CurrentProcess();
    }

    bool CoreProcess::WaitForDaemonFinished(
        const NekoGui_Runtime::DaemonProcessSnapshot& expected,
        std::chrono::milliseconds timeout,
        NekoGui_Runtime::DaemonProcessFinishedResult* result) const {
        return daemonProcessExit.WaitForFinished(expected, timeout, result);
    }

    NekoGui_Runtime::DaemonReadyResult CoreProcess::ConfirmDaemonReady(
        std::uint64_t expectedGeneration,
        const QString& expectedInstanceId) {
        return daemonGeneration.MarkProcessReady(
            expectedGeneration,
            expectedInstanceId.toStdString());
    }

    bool CoreProcess::IsDaemonReady(std::uint64_t daemonGenerationValue) const {
        return daemonGeneration.IsReady(daemonGenerationValue);
    }

    void CoreProcess::HandleCoreLog(QByteArray& probeBuffer, const QByteArray& log) {
        probeBuffer.append(log);
        if (probeBuffer.size() > 512) probeBuffer = probeBuffer.right(512);
        if (!probeBuffer.contains("grpc server listening")) return;
        probeBuffer.clear();

        RequestDaemonHandshake();
    }

    void CoreProcess::RequestDaemonHandshake() {
        // Process/log signals only prompt authentication. They are not
        // readiness proof: the loopback port may belong to an old daemon.
        const auto instance = daemonGeneration.CurrentInstance();
        if (!instance.valid() || instance.ready) return;
        MW_dialog_message(
            "CoreProcess",
            QStringLiteral("CoreListening,%1,%2,0")
                .arg(instance.generation)
                .arg(QString::fromStdString(instance.instanceId)));
    }

} // namespace NekoGui_sys
