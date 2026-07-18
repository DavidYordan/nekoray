#pragma once

#include <QProcess>

namespace NekoGui_sys {
    class CoreProcess : public QProcess {
    public:
        CoreProcess(const QString &core_path, const QStringList &args);

        void Start();

        void Restart();

        int start_profile_when_core_is_up = -1;

    private:
        QString program;
        QStringList arguments;
        QStringList env;

        bool started = false;
        bool show_stderr = false;
        bool failed_to_start = false;
        bool restarting = false;
    };

    inline QAtomicInt logCounter;
} // namespace NekoGui_sys
