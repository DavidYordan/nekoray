#pragma once

#include <QProcess>

#include "main/RuntimeTransition.hpp"

namespace NekoGui_sys {
    class CoreProcess : public QProcess {
    public:
        CoreProcess(const QString &core_path, const QStringList &args);

        void Start();

        void Restart();

        void EnsureStarted();

        [[nodiscard]] NekoGui_Runtime::DaemonProfileStartRequest
        QueueProfileStartWhenCoreIsUp(int profileId);

        [[nodiscard]] bool CancelQueuedProfileStart();

        [[nodiscard]] bool ConsumeQueuedProfileStart(
            std::uint64_t daemonGeneration,
            std::uint64_t requestGeneration,
            int profileId);

        [[nodiscard]] std::uint64_t CurrentDaemonGeneration() const;

        [[nodiscard]] bool IsDaemonReady(std::uint64_t daemonGeneration) const;

    private:
        QString program;
        QStringList arguments;
        QStringList env;

        bool started = false;
        bool show_stderr = false;
        bool failed_to_start = false;
        bool restarting = false;
        NekoGui_Runtime::DaemonGenerationState daemonGeneration;
    };

    inline QAtomicInt logCounter;
} // namespace NekoGui_sys
