#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace NekoGui_ConfigTransaction {
    struct FileState {
        bool exists = false;
        QByteArray content;
    };

    struct FileMutation {
        QString sourcePath;
        FileState before;
        FileState after;
    };

    enum class Outcome {
        PreparationFailed,
        Committed,
        RolledBack,
        RecoveryRequired,
    };

    struct Result {
        Outcome outcome = Outcome::PreparationFailed;
        QString transactionPath;
        QString error;

        [[nodiscard]] bool succeeded() const {
            return outcome == Outcome::Committed;
        }
    };

    [[nodiscard]] Result Execute(
        const QString &operation,
        const QList<FileMutation> &mutations);

    [[nodiscard]] QStringList BlockingTransactionIssues();

    [[nodiscard]] QString RuntimeMutationBlockReason();

#ifdef NEKORAY_CONFIG_TRANSACTION_TESTING
    void SetApplyFailureAfterForTest(int appliedMutationCount);
#endif
}
