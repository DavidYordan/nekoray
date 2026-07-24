#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace NekoGui_ConfigRecovery {
    struct SnapshotResult {
        bool succeeded = false;
        QString snapshotPath;
        QString error;
        QByteArray sha256;
    };

    enum class OverwriteDecision {
        Proceed,
        Unchanged,
        Refused,
    };

    struct OverwritePreparation {
        OverwriteDecision decision = OverwriteDecision::Refused;
        bool targetExisted = false;
        QString backupPath;
        QString error;
    };

    struct DeletionPreparation {
        bool ready = false;
        QString snapshotPath;
        QString error;
    };

    [[nodiscard]] OverwritePreparation PrepareOverwrite(
        const QString &sourcePath,
        const QByteArray &loadedContent,
        const QByteArray &newContent);

    [[nodiscard]] bool CurrentContentMatches(
        const QString &sourcePath,
        const QByteArray &expectedContent,
        QString *error);

    [[nodiscard]] SnapshotResult CreateBackupBeforeOverwrite(
        const QString &sourcePath,
        const QByteArray &sourceContent);

    [[nodiscard]] SnapshotResult RecordQuarantine(
        const QString &sourcePath,
        const QByteArray &sourceContent,
        const QString &reason);

    [[nodiscard]] SnapshotResult RecordPreDeletion(
        const QString &sourcePath,
        const QByteArray &sourceContent,
        const QString &reason);

    [[nodiscard]] DeletionPreparation PrepareDeletion(
        const QString &sourcePath,
        const QByteArray &loadedContent,
        const QString &reason);

    [[nodiscard]] QString RecoveryRootPath();

    [[nodiscard]] QStringList TakeRecoveryNotices();
}
