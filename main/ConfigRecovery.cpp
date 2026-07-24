#include "ConfigRecovery.hpp"
#include "ConfigPathSafety.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

namespace {
    QStringList recoveryNotices;
    QSet<QString> recoveryNoticeKeys;

    QByteArray sha256(const QByteArray& content) {
        return QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
    }

    bool relativeSourcePath(const QString& sourcePath, QString* relativePath, QString* error) {
        return NekoGui_ConfigPathSafety::RelativeConfigPath(sourcePath, relativePath, error);
    }

    bool readExact(const QString& path, QByteArray* content, QString* error) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Cannot read recovery file %1: %2").arg(path, file.errorString());
            return false;
        }
        *content = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            *error = QStringLiteral("Cannot read complete recovery file %1: %2").arg(path, file.errorString());
            return false;
        }
        return true;
    }

    bool writeAtomicExact(const QString& path, const QByteArray& content, QString* error) {
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                QDir::currentPath(), path, nullptr, error)) {
            return false;
        }
        const QFileInfo info(path);
        if (!NekoGui_ConfigPathSafety::EnsureDirectoryWithinRoot(
                QDir::currentPath(), info.absolutePath(), error)) {
            *error = QStringLiteral("Cannot prepare safe recovery directory %1: %2")
                         .arg(info.absolutePath(), *error);
            return false;
        }

        if (info.exists()) {
            QByteArray existing;
            if (!readExact(path, &existing, error)) return false;
            if (existing != content) {
                *error = QStringLiteral("Recovery path already exists with different content: %1").arg(path);
                return false;
            }
            return true;
        }

        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("Cannot open recovery snapshot for atomic write %1: %2")
                         .arg(path, file.errorString());
            return false;
        }
        if (file.write(content) != content.size()) {
            *error = QStringLiteral("Cannot write complete recovery snapshot %1: %2")
                         .arg(path, file.errorString());
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            *error = QStringLiteral("Cannot commit recovery snapshot %1: %2").arg(path, file.errorString());
            return false;
        }

        QByteArray verified;
        if (!readExact(path, &verified, error)) return false;
        if (verified != content) {
            *error = QStringLiteral("Recovery snapshot verification failed: %1").arg(path);
            return false;
        }
        return true;
    }

    bool replaceAtomicExact(
        const QString& path,
        const QByteArray& expectedContent,
        const QByteArray& newContent,
        QString* error) {
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                QDir::currentPath(), path, nullptr, error)) {
            return false;
        }
        QByteArray current;
        if (!readExact(path, &current, error)) return false;
        if (current != expectedContent) {
            *error = QStringLiteral("Recovery metadata changed before atomic replacement: %1").arg(path);
            return false;
        }
        if (current == newContent) return true;

        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("Cannot open recovery metadata for atomic replacement %1: %2")
                         .arg(path, file.errorString());
            return false;
        }
        if (file.write(newContent) != newContent.size()) {
            *error = QStringLiteral("Cannot write complete recovery metadata %1: %2")
                         .arg(path, file.errorString());
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            *error = QStringLiteral("Cannot commit recovery metadata %1: %2").arg(path, file.errorString());
            return false;
        }

        QByteArray verified;
        if (!readExact(path, &verified, error)) return false;
        if (verified != newContent) {
            *error = QStringLiteral("Recovery metadata verification failed: %1").arg(path);
            return false;
        }
        return true;
    }

    NekoGui_ConfigRecovery::SnapshotResult createSnapshot(
        const QString& category,
        const QString& suffix,
        const QString& sourcePath,
        const QByteArray& sourceContent) {
        NekoGui_ConfigRecovery::SnapshotResult result;
        result.sha256 = sha256(sourceContent);

        QString relative;
        if (!relativeSourcePath(sourcePath, &relative, &result.error)) return result;

        const QDir root(NekoGui_ConfigRecovery::RecoveryRootPath());
        result.snapshotPath = root.absoluteFilePath(
            QStringLiteral("%1/%2.%3%4")
                .arg(category, relative, QString::fromLatin1(result.sha256), suffix));
        if (!writeAtomicExact(result.snapshotPath, sourceContent, &result.error)) return result;

        result.succeeded = true;
        return result;
    }

    void addNotice(const QString& sourcePath, const QString& reason, const QString& snapshotPath) {
        const auto key = QFileInfo(sourcePath).absoluteFilePath() + "\n" + reason;
        if (recoveryNoticeKeys.contains(key)) return;
        recoveryNoticeKeys.insert(key);
        recoveryNotices.append(
            QStringLiteral("%1: %2 (snapshot: %3)").arg(sourcePath, reason, snapshotPath));
    }

    NekoGui_ConfigRecovery::SnapshotResult recordEvidence(
        const QString& category,
        const QString& schema,
        const QString& firstRecordedField,
        const QString& sourcePath,
        const QByteArray& sourceContent,
        const QString& reason,
        bool notifyUser) {
        auto result = createSnapshot(category, QStringLiteral(".snapshot"), sourcePath, sourceContent);
        if (!result.succeeded) return result;

        QString relative;
        if (!relativeSourcePath(sourcePath, &relative, &result.error)) {
            result.succeeded = false;
            return result;
        }

        const auto metadataPath = result.snapshotPath + QStringLiteral(".meta.json");
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                QDir::currentPath(), metadataPath, nullptr, &result.error)) {
            result.succeeded = false;
            return result;
        }
        QJsonObject metadata;
        QJsonArray reasons;
        QByteArray existingMetadata;
        if (QFileInfo::exists(metadataPath)) {
            if (!readExact(metadataPath, &existingMetadata, &result.error)) {
                result.succeeded = false;
                return result;
            }
            QJsonParseError parseError{};
            const auto document = QJsonDocument::fromJson(existingMetadata, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                result.error = QStringLiteral("Invalid recovery metadata %1: %2")
                                   .arg(metadataPath, parseError.errorString());
                result.succeeded = false;
                return result;
            }
            metadata = document.object();
            if (metadata.value(QStringLiteral("schema")).toString() != schema ||
                metadata.value(QStringLiteral("source_path")).toString() != relative ||
                metadata.value(QStringLiteral("sha256")).toString().toLatin1() != result.sha256) {
                result.error = QStringLiteral("Recovery metadata does not match its snapshot: %1").arg(metadataPath);
                result.succeeded = false;
                return result;
            }
            reasons = metadata.value(QStringLiteral("reasons")).toArray();
        } else {
            metadata.insert(QStringLiteral("schema"), schema);
            metadata.insert(QStringLiteral("source_path"), relative);
            metadata.insert(QStringLiteral("sha256"), QString::fromLatin1(result.sha256));
            metadata.insert(QStringLiteral("size"), static_cast<double>(sourceContent.size()));
            metadata.insert(
                firstRecordedField,
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            const QFileInfo sourceInfo(sourcePath);
            if (sourceInfo.exists()) {
                metadata.insert(
                    QStringLiteral("source_last_modified_utc"),
                    sourceInfo.lastModified().toUTC().toString(Qt::ISODateWithMs));
            }
        }

        bool reasonExists = false;
        for (const auto& value: reasons) {
            if (value.toString() == reason) {
                reasonExists = true;
                break;
            }
        }
        if (!reasonExists) {
            reasons.append(reason);
            metadata.insert(QStringLiteral("reasons"), reasons);
            const auto serialized = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
            const auto metadataWritten = existingMetadata.isEmpty()
                                             ? writeAtomicExact(metadataPath, serialized, &result.error)
                                             : replaceAtomicExact(metadataPath, existingMetadata, serialized, &result.error);
            if (!metadataWritten) {
                result.succeeded = false;
                return result;
            }
        }

        if (notifyUser) addNotice(sourcePath, reason, result.snapshotPath);
        return result;
    }
} // namespace

namespace NekoGui_ConfigRecovery {
    QString RecoveryRootPath() {
        return QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("recovery"));
    }

    bool CurrentContentMatches(
        const QString& sourcePath,
        const QByteArray& expectedContent,
        QString* error) {
        if (!NekoGui_ConfigPathSafety::RelativeConfigPath(sourcePath, nullptr, error)) return false;
        QByteArray current;
        if (!readExact(sourcePath, &current, error)) return false;
        if (current != expectedContent) {
            *error = QStringLiteral("Config changed outside this store: %1").arg(sourcePath);
            return false;
        }
        return true;
    }

    OverwritePreparation PrepareOverwrite(
        const QString& sourcePath,
        const QByteArray& loadedContent,
        const QByteArray& newContent) {
        OverwritePreparation result;
        if (!NekoGui_ConfigPathSafety::RelativeConfigPath(
                sourcePath, nullptr, &result.error)) {
            return result;
        }
        result.targetExisted = QFileInfo::exists(sourcePath);
        if (!result.targetExisted) {
            result.decision = OverwriteDecision::Proceed;
            return result;
        }
        if (loadedContent.isEmpty()) {
            result.error = QStringLiteral("Refusing to overwrite a config that was not loaded by this store: %1")
                               .arg(sourcePath);
            return result;
        }

        QByteArray current;
        if (!readExact(sourcePath, &current, &result.error)) return result;
        if (current != loadedContent) {
            result.error = QStringLiteral("Refusing to overwrite a config changed outside this store: %1")
                               .arg(sourcePath);
            return result;
        }
        if (current == newContent) {
            result.decision = OverwriteDecision::Unchanged;
            return result;
        }

        const auto backup = CreateBackupBeforeOverwrite(sourcePath, current);
        if (!backup.succeeded) {
            result.error = QStringLiteral("Verified pre-overwrite backup failed for %1: %2")
                               .arg(sourcePath, backup.error);
            return result;
        }
        result.backupPath = backup.snapshotPath;
        result.decision = OverwriteDecision::Proceed;
        return result;
    }

    SnapshotResult CreateBackupBeforeOverwrite(const QString& sourcePath, const QByteArray& sourceContent) {
        return createSnapshot(QStringLiteral("backups"), QStringLiteral(".bak"), sourcePath, sourceContent);
    }

    SnapshotResult RecordQuarantine(
        const QString& sourcePath,
        const QByteArray& sourceContent,
        const QString& reason) {
        return recordEvidence(
            QStringLiteral("quarantine"),
            QStringLiteral("nekoray.recovery.quarantine.v1"),
            QStringLiteral("first_observed_utc"),
            sourcePath,
            sourceContent,
            reason,
            true);
    }

    SnapshotResult RecordPreDeletion(
        const QString& sourcePath,
        const QByteArray& sourceContent,
        const QString& reason) {
        return recordEvidence(
            QStringLiteral("deletions"),
            QStringLiteral("nekoray.recovery.pre_delete.v1"),
            QStringLiteral("first_prepared_utc"),
            sourcePath,
            sourceContent,
            reason,
            false);
    }

    DeletionPreparation PrepareDeletion(
        const QString& sourcePath,
        const QByteArray& loadedContent,
        const QString& reason) {
        DeletionPreparation result;
        if (!QFileInfo::exists(sourcePath)) {
            result.error = QStringLiteral("Refusing to delete a config whose source file is missing: %1")
                               .arg(sourcePath);
            return result;
        }
        if (loadedContent.isEmpty()) {
            result.error = QStringLiteral("Refusing to delete a config that was not loaded by this store: %1")
                               .arg(sourcePath);
            return result;
        }

        QString verificationError;
        if (!CurrentContentMatches(sourcePath, loadedContent, &verificationError)) {
            result.error = QStringLiteral("Refusing to delete a config changed outside this store: %1")
                               .arg(verificationError);
            return result;
        }
        const auto deletion = RecordPreDeletion(sourcePath, loadedContent, reason);
        if (!deletion.succeeded) {
            result.error = QStringLiteral("Verified pre-deletion snapshot failed for %1: %2")
                               .arg(sourcePath, deletion.error);
            return result;
        }
        result.snapshotPath = deletion.snapshotPath;
        result.ready = true;
        return result;
    }

    QStringList TakeRecoveryNotices() {
        auto notices = recoveryNotices;
        recoveryNotices.clear();
        recoveryNoticeKeys.clear();
        return notices;
    }
} // namespace NekoGui_ConfigRecovery
