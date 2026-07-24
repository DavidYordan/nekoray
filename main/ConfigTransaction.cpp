#include "ConfigTransaction.hpp"

#include "ConfigMutation.hpp"
#include "ConfigPathSafety.hpp"
#include "ConfigRecovery.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QRecursiveMutex>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <cmath>
#include <limits>
#include <memory>

namespace {
    QString runtimeMutationBlockReason;
    QMutex transactionMutex;
    QRecursiveMutex diskLockMutex;
    std::unique_ptr<QLockFile> diskLockFile;
    int diskLockDepth = 0;
    QString diskLockRoot;

#ifdef NEKORAY_CONFIG_TRANSACTION_TESTING
    int applyFailureAfter = -1;
#endif

    QByteArray sha256(const QByteArray& content) {
        return QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
    }

    QString transactionsRootPath() {
        const auto configRoot = diskLockRoot.isEmpty()
                                    ? QFileInfo(QDir::currentPath()).absoluteFilePath()
                                    : diskLockRoot;
        return QDir(QDir(configRoot).absoluteFilePath(QStringLiteral("recovery")))
            .absoluteFilePath(QStringLiteral("transactions"));
    }

    bool sameConfigRoot(const QString& left, const QString& right) {
#ifdef Q_OS_WIN
        return left.compare(right, Qt::CaseInsensitive) == 0;
#else
        return left == right;
#endif
    }

    bool relativeSourcePath(const QString& sourcePath, QString* relativePath, QString* error) {
        return NekoGui_ConfigPathSafety::RelativeConfigPath(sourcePath, relativePath, error);
    }

    bool readExact(const QString& path, QByteArray* content, QString* error) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Cannot read transaction file %1: %2").arg(path, file.errorString());
            return false;
        }
        *content = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            *error = QStringLiteral("Cannot read complete transaction file %1: %2")
                         .arg(path, file.errorString());
            return false;
        }
        return true;
    }

    bool readState(const QString& path, NekoGui_ConfigTransaction::FileState* state, QString* error) {
        const QFileInfo info(path);
        if (!info.exists()) {
            state->exists = false;
            state->content.clear();
            return true;
        }
        if (!info.isFile()) {
            *error = QStringLiteral("Transaction target is not a regular file: %1").arg(path);
            return false;
        }
        state->exists = true;
        return readExact(path, &state->content, error);
    }

    bool statesEqual(
        const NekoGui_ConfigTransaction::FileState& left,
        const NekoGui_ConfigTransaction::FileState& right) {
        return left.exists == right.exists && (!left.exists || left.content == right.content);
    }

    bool currentStateMatches(
        const QString& path,
        const NekoGui_ConfigTransaction::FileState& expected,
        QString* error) {
        if (!relativeSourcePath(path, nullptr, error)) return false;
        NekoGui_ConfigTransaction::FileState current;
        if (!readState(path, &current, error)) return false;
        if (statesEqual(current, expected)) return true;

        *error = QStringLiteral("Transaction target changed outside the prepared state: %1").arg(path);
        return false;
    }

    bool writeAtomicExact(const QString& path, const QByteArray& content, QString* error) {
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                transactionsRootPath(), path, nullptr, error)) {
            return false;
        }
        const QFileInfo info(path);
        if (!NekoGui_ConfigPathSafety::EnsureDirectoryWithinRoot(
                transactionsRootPath(), info.absolutePath(), error)) {
            *error = QStringLiteral("Cannot prepare safe transaction directory %1: %2")
                         .arg(info.absolutePath(), *error);
            return false;
        }

        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("Cannot open transaction file for atomic write %1: %2")
                         .arg(path, file.errorString());
            return false;
        }
        if (file.write(content) != content.size()) {
            *error = QStringLiteral("Cannot write complete transaction file %1: %2")
                         .arg(path, file.errorString());
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            *error = QStringLiteral("Cannot commit transaction file %1: %2").arg(path, file.errorString());
            return false;
        }

        QByteArray verified;
        if (!readExact(path, &verified, error)) return false;
        if (verified != content) {
            *error = QStringLiteral("Transaction file verification failed: %1").arg(path);
            return false;
        }
        return true;
    }

    bool replaceAtomicExact(
        const QString& path,
        const QByteArray& expected,
        const QByteArray& replacement,
        QString* error) {
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                transactionsRootPath(), path, nullptr, error)) {
            return false;
        }
        QByteArray current;
        if (!readExact(path, &current, error)) return false;
        if (current != expected) {
            *error = QStringLiteral("Transaction manifest changed before state update: %1").arg(path);
            return false;
        }
        return writeAtomicExact(path, replacement, error);
    }

    bool writeTargetAtomicExact(
        const QString& path,
        const NekoGui_ConfigTransaction::FileState& expected,
        const QByteArray& content,
        bool* committed,
        QString* error) {
        *committed = false;
        if (!relativeSourcePath(path, nullptr, error)) return false;
        const QFileInfo info(path);
        if (!NekoGui_ConfigPathSafety::EnsureDirectoryWithinRoot(
                QDir::currentPath(), info.absolutePath(), error)) {
            *error = QStringLiteral("Cannot prepare safe transaction target directory %1: %2")
                         .arg(info.absolutePath(), *error);
            return false;
        }

        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("Cannot open transaction target for atomic write %1: %2")
                         .arg(path, file.errorString());
            return false;
        }
        if (file.write(content) != content.size()) {
            *error = QStringLiteral("Cannot write complete transaction target %1: %2")
                         .arg(path, file.errorString());
            file.cancelWriting();
            return false;
        }

        QString verificationError;
        if (!currentStateMatches(path, expected, &verificationError)) {
            *error = QStringLiteral("Transaction target changed while its replacement was prepared: %1")
                         .arg(verificationError);
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            *error = QStringLiteral("Cannot commit transaction target %1: %2").arg(path, file.errorString());
            return false;
        }
        *committed = true;

        QByteArray verified;
        if (!readExact(path, &verified, error)) return false;
        if (verified != content) {
            *error = QStringLiteral("Transaction target verification failed: %1").arg(path);
            return false;
        }
        return true;
    }

    QJsonObject stateJson(
        const NekoGui_ConfigTransaction::FileState& state,
        const QString& snapshotPath) {
        QJsonObject object;
        object.insert(QStringLiteral("exists"), state.exists);
        if (state.exists) {
            object.insert(QStringLiteral("sha256"), QString::fromLatin1(sha256(state.content)));
            object.insert(QStringLiteral("size"), static_cast<double>(state.content.size()));
            object.insert(QStringLiteral("snapshot"), snapshotPath);
        }
        return object;
    }

    struct ParsedRecoveryEntry {
        QString relativePath;
        QString sourcePath;
        NekoGui_ConfigTransaction::FileState before;
        NekoGui_ConfigTransaction::FileState after;
        NekoGui_ConfigTransaction::FileState current;
        QString currentLabel;
        QJsonObject manifestEntry;
    };

    struct ParsedRecoveryTransaction {
        QString id;
        QString transactionPath;
        QString manifestPath;
        QString operation;
        QString state;
        QString recoveryDirection;
        QByteArray manifestContent;
        QJsonObject manifest;
        QList<ParsedRecoveryEntry> entries;
        bool hasDivergedTarget = false;
    };

    bool isTerminalTransactionState(const QString& state) {
        return state == QStringLiteral("committed") ||
               state == QStringLiteral("rolled_back") ||
               state == QStringLiteral("aborted");
    }

    bool isRecoverableTransactionState(const QString& state) {
        return state == QStringLiteral("prepared") ||
               state == QStringLiteral("rollback_failed") ||
               state == QStringLiteral("recovering_before") ||
               state == QStringLiteral("recovering_after") ||
               state == QStringLiteral("recovery_failed");
    }

    bool validateRecoveryDirectionMetadata(
        const ParsedRecoveryTransaction& transaction,
        QString* lockedDirection,
        QString* error) {
        lockedDirection->clear();
        if (transaction.state == QStringLiteral("recovering_before")) {
            *lockedDirection = QStringLiteral("before");
            if (transaction.recoveryDirection != *lockedDirection) {
                *error = QStringLiteral(
                    "recovering_before requires recovery_direction 'before'.");
                return false;
            }
        } else if (transaction.state == QStringLiteral("recovering_after")) {
            *lockedDirection = QStringLiteral("after");
            if (transaction.recoveryDirection != *lockedDirection) {
                *error = QStringLiteral(
                    "recovering_after requires recovery_direction 'after'.");
                return false;
            }
        } else if (transaction.state == QStringLiteral("recovery_failed")) {
            if (transaction.recoveryDirection != QStringLiteral("before") &&
                transaction.recoveryDirection != QStringLiteral("after")) {
                *error = QStringLiteral(
                    "recovery_failed requires an exact before/after recovery direction.");
                return false;
            }
            *lockedDirection = transaction.recoveryDirection;
        } else if (transaction.state == QStringLiteral("prepared") ||
                   transaction.state == QStringLiteral("rollback_failed")) {
            if (!transaction.recoveryDirection.isEmpty()) {
                *error = QStringLiteral(
                    "A recovery direction is not valid before explicit recovery begins.");
                return false;
            }
        }
        return true;
    }

    bool isLowerHexSha256(const QString& value) {
        if (value.size() != 64) return false;
        for (const auto character: value) {
            const auto code = character.unicode();
            if ((code < '0' || code > '9') && (code < 'a' || code > 'f')) return false;
        }
        return true;
    }

    bool safeSnapshotPath(
        const QString& transactionPath,
        const QString& snapshot,
        const QString& requiredPrefix,
        QString* absolutePath,
        QString* error) {
        if (snapshot.isEmpty() || snapshot.contains('\\') || QDir::isAbsolutePath(snapshot) ||
            QDir::cleanPath(snapshot) != snapshot ||
            !snapshot.startsWith(requiredPrefix + QStringLiteral("/"))) {
            *error = QStringLiteral("Transaction snapshot path is unsafe: %1").arg(snapshot);
            return false;
        }

        const QDir transactionDirectory(transactionPath);
        const auto candidate = QFileInfo(transactionDirectory.absoluteFilePath(snapshot)).absoluteFilePath();
        auto relative = transactionDirectory.relativeFilePath(candidate);
        relative.replace('\\', '/');
        if (relative != snapshot || relative.startsWith(QStringLiteral("../"))) {
            *error = QStringLiteral("Transaction snapshot escapes its transaction directory: %1").arg(snapshot);
            return false;
        }
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                transactionPath, candidate, nullptr, error)) {
            return false;
        }
        *absolutePath = candidate;
        return true;
    }

    bool parseManifestFileState(
        const QJsonValue& value,
        const QString& transactionPath,
        const QString& snapshotPrefix,
        NekoGui_ConfigTransaction::FileState* state,
        QString* error) {
        if (!value.isObject()) {
            *error = QStringLiteral("Transaction entry state is not an object.");
            return false;
        }
        const auto object = value.toObject();
        const auto existsValue = object.value(QStringLiteral("exists"));
        if (!existsValue.isBool()) {
            *error = QStringLiteral("Transaction entry state has no boolean exists field.");
            return false;
        }

        state->exists = existsValue.toBool();
        state->content.clear();
        if (!state->exists) return true;

        const auto snapshot = object.value(QStringLiteral("snapshot")).toString();
        const auto expectedSha = object.value(QStringLiteral("sha256")).toString();
        const auto sizeValue = object.value(QStringLiteral("size"));
        if (!isLowerHexSha256(expectedSha) || !sizeValue.isDouble()) {
            *error = QStringLiteral("Transaction snapshot metadata is incomplete or invalid.");
            return false;
        }
        const auto expectedSize = sizeValue.toDouble(-1);
        if (expectedSize < 0 || std::floor(expectedSize) != expectedSize ||
            expectedSize > static_cast<double>(std::numeric_limits<qint64>::max())) {
            *error = QStringLiteral("Transaction snapshot size is invalid.");
            return false;
        }

        QString snapshotPath;
        if (!safeSnapshotPath(transactionPath, snapshot, snapshotPrefix, &snapshotPath, error)) return false;
        if (!readExact(snapshotPath, &state->content, error)) return false;
        if (static_cast<qint64>(state->content.size()) != static_cast<qint64>(expectedSize) ||
            QString::fromLatin1(sha256(state->content)) != expectedSha) {
            *error = QStringLiteral("Transaction snapshot hash or size does not match its manifest: %1")
                         .arg(snapshotPath);
            return false;
        }
        return true;
    }

    bool parseRecoveryTransaction(
        const QFileInfo& directory,
        ParsedRecoveryTransaction* transaction,
        QString* error) {
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                transactionsRootPath(), directory.absoluteFilePath(), nullptr, error)) {
            return false;
        }
        transaction->id = directory.fileName();
        transaction->transactionPath = directory.absoluteFilePath();
        transaction->manifestPath = QDir(transaction->transactionPath)
                                        .absoluteFilePath(QStringLiteral("manifest.json"));
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                transactionsRootPath(), transaction->manifestPath, nullptr, error)) {
            return false;
        }
        if (!readExact(transaction->manifestPath, &transaction->manifestContent, error)) return false;

        QJsonParseError parseError{};
        const auto document = QJsonDocument::fromJson(transaction->manifestContent, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            *error = QStringLiteral("Invalid transaction manifest: %1").arg(parseError.errorString());
            return false;
        }
        transaction->manifest = document.object();
        transaction->state = transaction->manifest.value(QStringLiteral("state")).toString();
        transaction->operation = transaction->manifest.value(QStringLiteral("operation")).toString();
        transaction->recoveryDirection = transaction->manifest.value(QStringLiteral("recovery_direction")).toString();
        if (transaction->manifest.value(QStringLiteral("schema")).toString() !=
                QStringLiteral("nekoray.config_transaction.v1") ||
            transaction->manifest.value(QStringLiteral("id")).toString() != transaction->id) {
            *error = QStringLiteral("Transaction identity or schema mismatch.");
            return false;
        }
        if (transaction->operation.trimmed().isEmpty() || transaction->state.isEmpty()) {
            *error = QStringLiteral("Transaction operation or state is missing.");
            return false;
        }

        const auto entriesValue = transaction->manifest.value(QStringLiteral("entries"));
        if (!entriesValue.isArray() || entriesValue.toArray().isEmpty()) {
            *error = QStringLiteral("Transaction manifest has no recovery entries.");
            return false;
        }

        QSet<QString> seenPaths;
        for (const auto& entryValue: entriesValue.toArray()) {
            if (!entryValue.isObject()) {
                *error = QStringLiteral("Transaction recovery entry is not an object.");
                return false;
            }
            ParsedRecoveryEntry entry;
            entry.manifestEntry = entryValue.toObject();
            entry.relativePath = entry.manifestEntry.value(QStringLiteral("path")).toString();
            if (entry.relativePath.isEmpty() || entry.relativePath.contains('\\') ||
                QDir::isAbsolutePath(entry.relativePath) ||
                QDir::cleanPath(entry.relativePath) != entry.relativePath) {
                *error = QStringLiteral("Transaction target path is unsafe: %1").arg(entry.relativePath);
                return false;
            }
            entry.sourcePath = QFileInfo(QDir::current().absoluteFilePath(entry.relativePath)).absoluteFilePath();
            QString normalizedRelative;
            if (!relativeSourcePath(entry.sourcePath, &normalizedRelative, error) ||
                normalizedRelative != entry.relativePath) {
                if (error->isEmpty()) {
                    *error = QStringLiteral("Transaction target path is not canonical: %1").arg(entry.relativePath);
                }
                return false;
            }
            const auto pathKey = entry.relativePath.toLower();
            if (seenPaths.contains(pathKey)) {
                *error = QStringLiteral("Transaction manifest contains a duplicate target: %1")
                             .arg(entry.relativePath);
                return false;
            }
            seenPaths.insert(pathKey);

            if (!parseManifestFileState(
                    entry.manifestEntry.value(QStringLiteral("before")),
                    transaction->transactionPath,
                    QStringLiteral("before"),
                    &entry.before,
                    error) ||
                !parseManifestFileState(
                    entry.manifestEntry.value(QStringLiteral("after")),
                    transaction->transactionPath,
                    QStringLiteral("after"),
                    &entry.after,
                    error)) {
                *error = QStringLiteral("%1: %2").arg(entry.relativePath, *error);
                return false;
            }
            if (statesEqual(entry.before, entry.after)) {
                *error = QStringLiteral("Transaction manifest contains a no-op target: %1")
                             .arg(entry.relativePath);
                return false;
            }
            if (!readState(entry.sourcePath, &entry.current, error)) return false;
            if (statesEqual(entry.current, entry.before)) {
                entry.currentLabel = QStringLiteral("before");
            } else if (statesEqual(entry.current, entry.after)) {
                entry.currentLabel = QStringLiteral("after");
            } else {
                entry.currentLabel = QStringLiteral("diverged");
                transaction->hasDivergedTarget = true;
            }
            transaction->entries.append(entry);
        }
        return true;
    }

    QByteArray manifestObjectBytes(const QJsonObject& manifest) {
        return QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    }

    bool updateRecoveryManifest(
        ParsedRecoveryTransaction* transaction,
        const QString& state,
        const QString& direction,
        const QString& detail,
        bool completed,
        QString* error) {
        auto replacement = transaction->manifest;
        replacement.insert(QStringLiteral("state"), state);
        replacement.insert(QStringLiteral("updated_utc"),
                           QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        replacement.insert(QStringLiteral("recovery_direction"), direction);
        if (!replacement.contains(QStringLiteral("recovery_started_utc"))) {
            replacement.insert(QStringLiteral("recovery_started_utc"),
                               QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        }
        if (completed) {
            replacement.insert(QStringLiteral("recovered_utc"),
                               QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        }
        if (detail.isEmpty()) {
            replacement.remove(QStringLiteral("detail"));
        } else {
            replacement.insert(QStringLiteral("detail"), detail);
        }

        const auto replacementContent = manifestObjectBytes(replacement);
        if (!replaceAtomicExact(
                transaction->manifestPath,
                transaction->manifestContent,
                replacementContent,
                error)) {
            return false;
        }
        transaction->manifest = replacement;
        transaction->manifestContent = replacementContent;
        transaction->state = state;
        transaction->recoveryDirection = direction;
        return true;
    }

    QByteArray manifestBytes(
        const QString& transactionId,
        const QString& operation,
        const QString& state,
        const QJsonArray& entries,
        const QString& detail = {}) {
        QJsonObject manifest;
        manifest.insert(QStringLiteral("schema"), QStringLiteral("nekoray.config_transaction.v1"));
        manifest.insert(QStringLiteral("id"), transactionId);
        manifest.insert(QStringLiteral("operation"), operation);
        manifest.insert(QStringLiteral("state"), state);
        manifest.insert(QStringLiteral("updated_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        manifest.insert(QStringLiteral("entries"), entries);
        if (!detail.isEmpty()) manifest.insert(QStringLiteral("detail"), detail);
        return QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    }

    bool applyState(
        const QString& path,
        const NekoGui_ConfigTransaction::FileState& expected,
        const NekoGui_ConfigTransaction::FileState& desired,
        bool* mutationMayHaveApplied,
        QString* error) {
        *mutationMayHaveApplied = false;
        if (!relativeSourcePath(path, nullptr, error)) return false;
        if (!currentStateMatches(path, expected, error)) return false;

        if (desired.exists) {
            bool committed = false;
            const auto written = writeTargetAtomicExact(path, expected, desired.content, &committed, error);
            *mutationMayHaveApplied = committed;
            if (!written) return false;
        } else if (QFileInfo::exists(path) && !QFile::remove(path)) {
            *error = QStringLiteral("Cannot delete transaction target: %1").arg(path);
            return false;
        } else {
            *mutationMayHaveApplied = true;
        }

        if (!currentStateMatches(path, desired, error)) {
            *error = QStringLiteral("Transaction target verification failed after mutation: %1 (%2)")
                         .arg(path, *error);
            return false;
        }
        return true;
    }

    bool rollbackApplied(
        const QList<NekoGui_ConfigTransaction::FileMutation>& mutations,
        const QList<int>& applied,
        QString* error) {
        QStringList failures;
        for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
            const auto& mutation = mutations.at(*it);
            QString rollbackError;
            bool rollbackMayHaveApplied = false;
            if (!applyState(
                    mutation.sourcePath,
                    mutation.after,
                    mutation.before,
                    &rollbackMayHaveApplied,
                    &rollbackError)) {
                failures.append(QStringLiteral("%1: %2").arg(mutation.sourcePath, rollbackError));
            }
        }
        if (failures.isEmpty()) return true;
        *error = failures.join(QStringLiteral("; "));
        return false;
    }

    bool updateManifestState(
        const QString& manifestPath,
        QByteArray* currentManifest,
        const QString& transactionId,
        const QString& operation,
        const QString& state,
        const QJsonArray& entries,
        const QString& detail,
        QString* error) {
        const auto replacement = manifestBytes(transactionId, operation, state, entries, detail);
        if (!replaceAtomicExact(manifestPath, *currentManifest, replacement, error)) return false;
        *currentManifest = replacement;
        return true;
    }

    QFileInfoList transactionRootEntries() {
        return QDir(transactionsRootPath())
            .entryInfoList(
                QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                QDir::Name);
    }

    bool isActiveLockEntry(const QFileInfo& entry) {
        return entry.fileName() == QStringLiteral("active.lock") && entry.isFile();
    }

    bool isUnpublishedStagingEntry(const QFileInfo& entry) {
        const auto prefix = QStringLiteral(".staging-");
        if (!entry.isDir() || !entry.fileName().startsWith(prefix, Qt::CaseSensitive)) {
            return false;
        }
        const auto id = entry.fileName().mid(prefix.size());
        const QUuid uuid(id);
        return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
    }

    bool removeUnpublishedStagingDirectory(const QString& path, QString* error) {
        const QFileInfo entry(path);
        if (!entry.exists()) return true;
        if (!isUnpublishedStagingEntry(entry) ||
            !NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                transactionsRootPath(), entry.absoluteFilePath(), nullptr, error)) {
            if (error->isEmpty()) {
                *error = QStringLiteral("Refusing to remove an unsafe transaction staging path: %1")
                             .arg(path);
            }
            return false;
        }
        if (!QDir(entry.absoluteFilePath()).removeRecursively()) {
            *error = QStringLiteral("Cannot remove unpublished transaction staging directory: %1")
                         .arg(entry.absoluteFilePath());
            return false;
        }
        return true;
    }

    bool retireTerminalSingleFileTransaction(
        const QString& transactionPath,
        const QString& transactionId,
        QString* error) {
        const QFileInfo source(transactionPath);
        if (!source.exists() || !source.isDir() || source.fileName() != transactionId ||
            !NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                transactionsRootPath(), source.absoluteFilePath(), nullptr, error)) {
            if (error->isEmpty()) {
                *error = QStringLiteral("Refusing to retire an unsafe transaction path: %1")
                             .arg(transactionPath);
            }
            return false;
        }

        const auto retirementRoot = QDir(NekoGui_ConfigRecovery::RecoveryRootPath())
                                        .absoluteFilePath(QStringLiteral("retired-single-file-transactions"));
        if (!NekoGui_ConfigPathSafety::EnsureDirectoryWithinRoot(
                QDir::currentPath(), retirementRoot, error)) {
            return false;
        }
        const auto retiredPath = QDir(retirementRoot).absoluteFilePath(transactionId);
        if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                QDir::currentPath(), retiredPath, nullptr, error)) {
            return false;
        }
        if (QFileInfo::exists(retiredPath)) {
            *error = QStringLiteral("Single-file transaction retirement path already exists: %1")
                         .arg(retiredPath);
            return false;
        }
        if (!QDir().rename(source.absoluteFilePath(), retiredPath)) {
            *error = QStringLiteral("Cannot atomically retire terminal single-file transaction: %1")
                         .arg(transactionPath);
            return false;
        }
        if (!QDir(retiredPath).removeRecursively()) {
            *error = QStringLiteral("Retired single-file transaction could not be deleted: %1")
                         .arg(retiredPath);
            return false;
        }
        return true;
    }

    QStringList scanTransactionIssues() {
        QStringList issues;
        const QDir root(transactionsRootPath());
        if (!root.exists()) return issues;

        for (const auto& entry: transactionRootEntries()) {
            QString pathError;
            if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                    transactionsRootPath(), entry.absoluteFilePath(), nullptr, &pathError)) {
                issues.append(QStringLiteral("%1: %2").arg(entry.absoluteFilePath(), pathError));
                continue;
            }
            if (isActiveLockEntry(entry)) continue;
            if (isUnpublishedStagingEntry(entry)) continue;
            if (!entry.isDir()) {
                issues.append(QStringLiteral("%1: unexpected file in the transaction root")
                                  .arg(entry.absoluteFilePath()));
                continue;
            }
            const auto manifestPath = QDir(entry.absoluteFilePath()).absoluteFilePath(QStringLiteral("manifest.json"));
            QString error;
            if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                    transactionsRootPath(), manifestPath, nullptr, &error)) {
                issues.append(QStringLiteral("%1: %2").arg(entry.absoluteFilePath(), error));
                continue;
            }
            QByteArray bytes;
            if (!readExact(manifestPath, &bytes, &error)) {
                issues.append(QStringLiteral("%1: %2").arg(entry.absoluteFilePath(), error));
                continue;
            }

            QJsonParseError parseError{};
            const auto document = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                issues.append(
                    QStringLiteral("%1: invalid transaction manifest: %2")
                        .arg(entry.absoluteFilePath(), parseError.errorString()));
                continue;
            }

            const auto manifest = document.object();
            const auto schema = manifest.value(QStringLiteral("schema")).toString();
            const auto id = manifest.value(QStringLiteral("id")).toString();
            const auto state = manifest.value(QStringLiteral("state")).toString();
            if (schema != QStringLiteral("nekoray.config_transaction.v1") || id != entry.fileName()) {
                issues.append(QStringLiteral("%1: transaction identity or schema mismatch")
                                  .arg(entry.absoluteFilePath()));
                continue;
            }
            if (state == "committed" || state == "rolled_back" || state == "aborted") continue;

            issues.append(
                QStringLiteral("%1: transaction state '%2' requires explicit recovery")
                    .arg(entry.absoluteFilePath(), state.isEmpty() ? QStringLiteral("missing") : state));
        }
        return issues;
    }
} // namespace

namespace NekoGui_ConfigTransaction {
    DiskLockGuard::DiskLockGuard() {
        if (!diskLockMutex.tryLock()) {
            failure = QStringLiteral(
                "Another thread owns the in-process configuration disk lock.");
            return;
        }

        const auto currentRoot = QFileInfo(QDir::currentPath()).absoluteFilePath();
        if (diskLockDepth == 0) {
            diskLockRoot = currentRoot;
            QString directoryError;
            if (!NekoGui_ConfigPathSafety::EnsureDirectoryWithinRoot(
                    diskLockRoot, transactionsRootPath(), &directoryError)) {
                failure = QStringLiteral("Cannot prepare the transaction lock directory: %1")
                              .arg(directoryError);
                diskLockRoot.clear();
                diskLockMutex.unlock();
                return;
            }

            const auto lockPath = QDir(transactionsRootPath())
                                      .absoluteFilePath(QStringLiteral("active.lock"));
            if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                    transactionsRootPath(), lockPath, nullptr, &directoryError)) {
                failure = QStringLiteral("The transaction lock path is unsafe: %1")
                              .arg(directoryError);
                diskLockRoot.clear();
                diskLockMutex.unlock();
                return;
            }
            auto candidate = std::make_unique<QLockFile>(lockPath);
            // A valid transaction can take longer than Qt's 30-second default.
            // Disable age-only lock stealing; dead-owner detection remains.
            candidate->setStaleLockTime(0);
            if (!candidate->tryLock(0)) {
                failure = QStringLiteral("Another process owns the configuration transaction lock: %1")
                              .arg(candidate->fileName());
                diskLockRoot.clear();
                diskLockMutex.unlock();
                return;
            }
            diskLockFile = std::move(candidate);
        } else if (!sameConfigRoot(diskLockRoot, currentRoot)) {
            failure = QStringLiteral(
                          "The process configuration root changed while its disk lock was held: %1 -> %2")
                          .arg(diskLockRoot, currentRoot);
            diskLockMutex.unlock();
            return;
        }

        ++diskLockDepth;
        locked = true;
    }

    DiskLockGuard::~DiskLockGuard() {
        if (!locked) return;
        --diskLockDepth;
        if (diskLockDepth == 0) {
            if (diskLockFile != nullptr) diskLockFile->unlock();
            diskLockFile.reset();
            diskLockRoot.clear();
        }
        diskLockMutex.unlock();
    }

    bool DiskLockGuard::acquired() const {
        return locked;
    }

    QString DiskLockGuard::error() const {
        return failure;
    }

    QString RuntimeMutationBlockReason() {
        QMutexLocker lock(&transactionMutex);
        return runtimeMutationBlockReason;
    }

    QStringList BlockingTransactionIssues() {
        DiskLockGuard diskLock;
        if (!diskLock.acquired()) return {diskLock.error()};
        return scanTransactionIssues();
    }

    RecoveryReportResult BuildRecoveryReport() {
        QMutexLocker processLock(&transactionMutex);
        RecoveryReportResult result;

        QJsonObject report;
        report.insert(QStringLiteral("schema"),
                      QStringLiteral("nekoray.config_transaction_recovery_report.v1"));
        report.insert(QStringLiteral("generated_utc"),
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        report.insert(QStringLiteral("config_root"), QFileInfo(QDir::currentPath()).absoluteFilePath());
        QJsonArray transactions;

        DiskLockGuard diskLock;
        if (!diskLock.acquired()) {
            result.error = diskLock.error();
            return result;
        }

        for (const auto& entry: transactionRootEntries()) {
            QString rootEntryError;
            if (!NekoGui_ConfigPathSafety::ValidatePathWithinRoot(
                    transactionsRootPath(), entry.absoluteFilePath(), nullptr, &rootEntryError)) {
                // Keep the validation error for the report below.
            } else if (isActiveLockEntry(entry)) {
                continue;
            } else if (isUnpublishedStagingEntry(entry)) {
                continue;
            } else if (!entry.isDir()) {
                rootEntryError = QStringLiteral("Unexpected file in the transaction root.");
            }

            if (!rootEntryError.isEmpty()) {
                QJsonObject item;
                item.insert(QStringLiteral("id"), entry.fileName());
                item.insert(QStringLiteral("transaction_path"), entry.absoluteFilePath());
                item.insert(QStringLiteral("valid"), false);
                item.insert(QStringLiteral("recoverable"), false);
                item.insert(QStringLiteral("entries"), QJsonArray{});
                item.insert(QStringLiteral("error"), rootEntryError);
                transactions.append(item);
                continue;
            }

            ParsedRecoveryTransaction transaction;
            QString parseError;
            const auto parsed = parseRecoveryTransaction(entry, &transaction, &parseError);
            if (parsed && isTerminalTransactionState(transaction.state)) continue;

            QJsonObject item;
            item.insert(QStringLiteral("id"), entry.fileName());
            item.insert(QStringLiteral("transaction_path"), entry.absoluteFilePath());
            item.insert(QStringLiteral("valid"), parsed);
            if (!transaction.operation.isEmpty()) {
                item.insert(QStringLiteral("operation"), transaction.operation);
            }
            if (!transaction.state.isEmpty()) item.insert(QStringLiteral("state"), transaction.state);
            if (!transaction.recoveryDirection.isEmpty()) {
                item.insert(QStringLiteral("recovery_direction"), transaction.recoveryDirection);
            }

            bool directionMetadataValid = true;
            QString lockedDirection;
            QString directionMetadataError;
            if (parsed) {
                directionMetadataValid = validateRecoveryDirectionMetadata(
                    transaction, &lockedDirection, &directionMetadataError);
                Q_UNUSED(lockedDirection);
            }
            const auto recoverable = parsed && isRecoverableTransactionState(transaction.state) &&
                                     !transaction.hasDivergedTarget && directionMetadataValid;
            item.insert(QStringLiteral("recoverable"), recoverable);

            QJsonArray entries;
            if (parsed) {
                for (const auto& entry: transaction.entries) {
                    QJsonObject entryReport;
                    entryReport.insert(QStringLiteral("path"), entry.relativePath);
                    entryReport.insert(QStringLiteral("current"), entry.currentLabel);
                    entryReport.insert(QStringLiteral("before"),
                                       entry.manifestEntry.value(QStringLiteral("before")));
                    entryReport.insert(QStringLiteral("after"),
                                       entry.manifestEntry.value(QStringLiteral("after")));
                    entries.append(entryReport);
                }
            }
            item.insert(QStringLiteral("entries"), entries);

            QString reportError = parseError;
            if (parsed && !isRecoverableTransactionState(transaction.state)) {
                reportError = QStringLiteral("Transaction state is not eligible for manual recovery.");
            } else if (parsed && transaction.hasDivergedTarget) {
                reportError = QStringLiteral(
                    "At least one target matches neither the durable before nor after snapshot.");
            } else if (parsed && !directionMetadataValid) {
                reportError = QStringLiteral("Transaction recovery direction metadata is inconsistent: %1")
                                  .arg(directionMetadataError);
            }
            if (!reportError.isEmpty()) item.insert(QStringLiteral("error"), reportError);
            transactions.append(item);
        }

        report.insert(QStringLiteral("transactions"), transactions);
        result.json = QJsonDocument(report).toJson(QJsonDocument::Indented);
        result.succeeded = true;
        return result;
    }

    ManualRecoveryResult Recover(const QString& transactionId, RecoveryDirection direction) {
        NekoGui_ConfigMutation::Guard mutationGuard(false);
        ManualRecoveryResult result;
        if (!mutationGuard.acquired()) {
            result.error = QStringLiteral(
                "Manual recovery was refused while another model mutation is committing.");
            return result;
        }
        QMutexLocker processLock(&transactionMutex);
        const auto normalizedId = transactionId.trimmed();
        if (normalizedId.isEmpty() || normalizedId != transactionId || normalizedId.size() > 128 ||
            normalizedId == QStringLiteral(".") || normalizedId == QStringLiteral("..") ||
            normalizedId.startsWith(QStringLiteral(".staging-"), Qt::CaseInsensitive)) {
            result.error = QStringLiteral("Manual recovery requires an exact safe transaction id.");
            return result;
        }
        for (const auto character: normalizedId) {
            if (!character.isLetterOrNumber() && character != '-' && character != '_') {
                result.error = QStringLiteral("Manual recovery transaction id contains an unsafe character.");
                return result;
            }
        }

        DiskLockGuard diskLock;
        if (!diskLock.acquired()) {
            result.error = diskLock.error();
            return result;
        }
        QFileInfo directory;
        for (const auto& entry: transactionRootEntries()) {
            if (entry.isDir() &&
                entry.fileName().compare(normalizedId, Qt::CaseSensitive) == 0) {
                directory = entry;
                break;
            }
        }
        if (!directory.exists()) {
            result.error = QStringLiteral("Configuration transaction does not exist: %1").arg(normalizedId);
            return result;
        }
        result.transactionPath = directory.absoluteFilePath();

        ParsedRecoveryTransaction transaction;
        if (!parseRecoveryTransaction(directory, &transaction, &result.error)) return result;
        if (!isRecoverableTransactionState(transaction.state)) {
            result.error = QStringLiteral("Transaction %1 is in state '%2' and is not recoverable.")
                               .arg(normalizedId, transaction.state);
            return result;
        }
        QString lockedDirection;
        QString directionMetadataError;
        if (!validateRecoveryDirectionMetadata(
                transaction, &lockedDirection, &directionMetadataError)) {
            result.error = QStringLiteral(
                               "Transaction recovery direction metadata is inconsistent: %1")
                               .arg(directionMetadataError);
            return result;
        }
        if (transaction.hasDivergedTarget) {
            QStringList diverged;
            for (const auto& entry: transaction.entries) {
                if (entry.currentLabel == QStringLiteral("diverged")) diverged.append(entry.relativePath);
            }
            result.error = QStringLiteral(
                               "Manual recovery refused because targets match neither snapshot: %1")
                               .arg(diverged.join(QStringLiteral(", ")));
            return result;
        }

        const auto directionName = direction == RecoveryDirection::RestoreBefore
                                       ? QStringLiteral("before")
                                       : QStringLiteral("after");
        if (!lockedDirection.isEmpty() && lockedDirection != directionName) {
            result.error = QStringLiteral(
                               "Transaction recovery is already locked to '%1'; refusing requested '%2'.")
                               .arg(lockedDirection.isEmpty() ? QStringLiteral("invalid") : lockedDirection,
                                    directionName);
            return result;
        }

        QString manifestError;
        if (!updateRecoveryManifest(
                &transaction,
                direction == RecoveryDirection::RestoreBefore
                    ? QStringLiteral("recovering_before")
                    : QStringLiteral("recovering_after"),
                directionName,
                QStringLiteral("Explicit manual recovery started."),
                false,
                &manifestError)) {
            result.error = QStringLiteral("Cannot persist manual recovery direction: %1").arg(manifestError);
            runtimeMutationBlockReason = result.error;
            return result;
        }

        auto recordRecoveryFailure = [&](const QString& detail) {
            QString markerError;
            updateRecoveryManifest(
                &transaction,
                QStringLiteral("recovery_failed"),
                directionName,
                detail,
                false,
                &markerError);
            result.error = detail;
            if (!markerError.isEmpty()) {
                result.error += QStringLiteral("; recovery failure marker failed: %1")
                                    .arg(markerError);
            }
            runtimeMutationBlockReason = result.error;
        };

        for (const auto& entry: transaction.entries) {
            const auto& desired = direction == RecoveryDirection::RestoreBefore ? entry.before : entry.after;
            if (statesEqual(entry.current, desired)) {
                QString verificationError;
                if (!currentStateMatches(entry.sourcePath, desired, &verificationError)) {
                    recordRecoveryFailure(
                        QStringLiteral("Manual recovery target changed before it could be confirmed at %1: %2")
                            .arg(entry.relativePath, verificationError));
                    return result;
                }
                continue;
            }

            QString applyError;
            bool mutationMayHaveApplied = false;
            if (!applyState(
                    entry.sourcePath,
                    entry.current,
                    desired,
                    &mutationMayHaveApplied,
                    &applyError)) {
                const auto detail = QStringLiteral("Manual recovery failed at %1: %2")
                                        .arg(entry.relativePath, applyError);
                recordRecoveryFailure(detail);
                return result;
            }
        }

        QStringList finalVerificationFailures;
        for (const auto& entry: transaction.entries) {
            const auto& desired = direction == RecoveryDirection::RestoreBefore ? entry.before : entry.after;
            QString verificationError;
            if (!currentStateMatches(entry.sourcePath, desired, &verificationError)) {
                finalVerificationFailures.append(
                    QStringLiteral("%1: %2").arg(entry.relativePath, verificationError));
            }
        }
        if (!finalVerificationFailures.isEmpty()) {
            recordRecoveryFailure(
                QStringLiteral("Manual recovery final verification failed: %1")
                    .arg(finalVerificationFailures.join(QStringLiteral("; "))));
            return result;
        }

        result.finalState = direction == RecoveryDirection::RestoreBefore
                                ? QStringLiteral("rolled_back")
                                : QStringLiteral("committed");
        const auto completionDetail = direction == RecoveryDirection::RestoreBefore
                                          ? QStringLiteral("Manual recovery explicitly restored all before states.")
                                          : QStringLiteral("Manual recovery explicitly applied all after states.");
        if (!updateRecoveryManifest(
                &transaction,
                result.finalState,
                directionName,
                completionDetail,
                true,
                &manifestError)) {
            result.error = QStringLiteral(
                               "All targets reached the selected state, but the recovery marker failed: %1")
                               .arg(manifestError);
            runtimeMutationBlockReason = result.error;
            return result;
        }

        const auto remainingIssues = scanTransactionIssues();
        runtimeMutationBlockReason = remainingIssues.isEmpty()
                                         ? QString{}
                                         : QStringLiteral("Another configuration transaction still requires recovery: %1")
                                               .arg(remainingIssues.join(QStringLiteral(" | ")));
        result.succeeded = true;
        return result;
    }

    MutationIntentResult PrepareMutationIntent(
        const QString& operation,
        const FileMutation& mutation) {
        MutationIntentResult result;
        NekoGui_ConfigMutation::Guard mutationGuard(false);
        if (!mutationGuard.acquired()) {
            result.error = QStringLiteral(
                "Cannot acquire the model lock to prepare the single-file mutation intent.");
            return result;
        }
        QMutexLocker processLock(&transactionMutex);
        if (!runtimeMutationBlockReason.isEmpty()) {
            result.error = runtimeMutationBlockReason;
            return result;
        }
        DiskLockGuard diskLock;
        if (!diskLock.acquired()) {
            result.error = diskLock.error();
            return result;
        }

        const auto existingIssues = scanTransactionIssues();
        if (!existingIssues.isEmpty()) {
            result.error = QStringLiteral("A previous configuration transaction already requires recovery: %1")
                               .arg(existingIssues.join(QStringLiteral(" | ")));
            return result;
        }
        const auto normalizedOperation = operation.trimmed();
        if (normalizedOperation.isEmpty() || statesEqual(mutation.before, mutation.after)) {
            result.error = QStringLiteral("The single-file mutation intent has invalid operation or file states.");
            return result;
        }
        QString relativePath;
        QString pathError;
        if (!relativeSourcePath(mutation.sourcePath, &relativePath, &pathError)) {
            result.error = pathError;
            return result;
        }
        if (!currentStateMatches(mutation.sourcePath, mutation.before, &pathError)) {
            result.error = pathError;
            return result;
        }

        const QDir transactionsRoot(transactionsRootPath());
        const auto transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto stagingName = QStringLiteral(".staging-%1").arg(transactionId);
        const auto stagingPath = transactionsRoot.absoluteFilePath(stagingName);
        QString markerError;
        if (!NekoGui_ConfigPathSafety::EnsureDirectoryWithinRoot(
                transactionsRootPath(), stagingPath, &markerError)) {
            result.error = QStringLiteral("Cannot create single-file intent staging: %1").arg(markerError);
            return result;
        }
        auto abandonStaging = [&] {
            QString cleanupError;
            if (!removeUnpublishedStagingDirectory(stagingPath, &cleanupError)) {
                markerError += QStringLiteral("; staging cleanup failed: %1").arg(cleanupError);
            }
        };

        QString beforeSnapshot;
        QString afterSnapshot;
        if (mutation.before.exists) {
            beforeSnapshot = QStringLiteral("before/0000.bin");
            if (!writeAtomicExact(
                    QDir(stagingPath).absoluteFilePath(beforeSnapshot),
                    mutation.before.content,
                    &markerError)) {
                abandonStaging();
                result.error = markerError;
                return result;
            }
        }
        if (mutation.after.exists) {
            afterSnapshot = QStringLiteral("after/0000.bin");
            if (!writeAtomicExact(
                    QDir(stagingPath).absoluteFilePath(afterSnapshot),
                    mutation.after.content,
                    &markerError)) {
                abandonStaging();
                result.error = markerError;
                return result;
            }
        }

        QJsonObject entry;
        entry.insert(QStringLiteral("path"), relativePath);
        entry.insert(QStringLiteral("before"), stateJson(mutation.before, beforeSnapshot));
        entry.insert(QStringLiteral("after"), stateJson(mutation.after, afterSnapshot));
        QJsonArray entries;
        entries.append(entry);
        const auto manifest = manifestBytes(
            transactionId,
            normalizedOperation,
            QStringLiteral("prepared"),
            entries,
            QStringLiteral("Durable single-file atomic-save intent published before commit."));
        if (!writeAtomicExact(
                QDir(stagingPath).absoluteFilePath(QStringLiteral("manifest.json")),
                manifest,
                &markerError)) {
            abandonStaging();
            result.error = markerError;
            return result;
        }
        if (!QDir(transactionsRoot.absolutePath()).rename(stagingName, transactionId)) {
            markerError = QStringLiteral("Cannot publish the single-file mutation intent.");
            abandonStaging();
            result.error = markerError;
            return result;
        }

        result.transactionId = transactionId;
        result.transactionPath = transactionsRoot.absoluteFilePath(transactionId);
        result.succeeded = true;
        return result;
    }

    MutationIntentCompletionResult CompleteMutationIntent(
        const QString& transactionId,
        MutationIntentDisposition disposition,
        const QString& detail) {
        MutationIntentCompletionResult result;
        NekoGui_ConfigMutation::Guard mutationGuard(false);
        if (!mutationGuard.acquired()) {
            result.error = QStringLiteral(
                "Cannot acquire the model lock to complete the single-file mutation intent.");
            return result;
        }
        QMutexLocker processLock(&transactionMutex);
        auto block = [&](const QString& error) {
            result.error = error;
            runtimeMutationBlockReason = QStringLiteral(
                                             "A single-file atomic save intent requires explicit recovery: %1")
                                             .arg(error);
        };
        DiskLockGuard diskLock;
        if (!diskLock.acquired()) {
            block(diskLock.error());
            return result;
        }

        QFileInfo directory;
        for (const auto& entry: transactionRootEntries()) {
            if (entry.isDir() &&
                entry.fileName().compare(transactionId, Qt::CaseSensitive) == 0) {
                directory = entry;
                break;
            }
        }
        if (!directory.exists()) {
            block(QStringLiteral("Single-file mutation intent does not exist: %1")
                      .arg(transactionId));
            return result;
        }

        ParsedRecoveryTransaction transaction;
        QString parseError;
        if (!parseRecoveryTransaction(directory, &transaction, &parseError)) {
            block(parseError);
            return result;
        }
        if (transaction.state != QStringLiteral("prepared") || transaction.entries.size() != 1) {
            block(QStringLiteral("Single-file mutation intent has an invalid state or entry count."));
            return result;
        }
        if (disposition == MutationIntentDisposition::Indeterminate) {
            block(detail.trimmed().isEmpty()
                      ? QStringLiteral("Atomic replacement outcome could not be verified.")
                      : detail.trimmed());
            return result;
        }

        const auto& intentEntry = transaction.entries.first();
        const auto& expected = disposition == MutationIntentDisposition::VerifiedAfter
                                   ? intentEntry.after
                                   : intentEntry.before;
        QString verificationError;
        if (!currentStateMatches(intentEntry.sourcePath, expected, &verificationError)) {
            block(QStringLiteral("Single-file mutation completion verification failed: %1")
                      .arg(verificationError));
            return result;
        }

        result.finalState = disposition == MutationIntentDisposition::VerifiedAfter
                                ? QStringLiteral("committed")
                                : QStringLiteral("aborted");
        const auto entries = transaction.manifest.value(QStringLiteral("entries")).toArray();
        QString markerError;
        if (!updateManifestState(
                transaction.manifestPath,
                &transaction.manifestContent,
                transaction.id,
                transaction.operation,
                result.finalState,
                entries,
                detail,
                &markerError)) {
            block(QStringLiteral("Target was verified but the intent completion marker failed: %1")
                      .arg(markerError));
            return result;
        }

        result.resolved = true;
        const auto remainingIssues = scanTransactionIssues();
        runtimeMutationBlockReason = remainingIssues.isEmpty()
                                         ? QString{}
                                         : QStringLiteral("Another configuration transaction still requires recovery: %1")
                                               .arg(remainingIssues.join(QStringLiteral(" | ")));
        QString retirementError;
        if (!retireTerminalSingleFileTransaction(
                transaction.transactionPath, transaction.id, &retirementError)) {
            result.error = retirementError;
        }
        return result;
    }

    Result Execute(const QString& operation, const QList<FileMutation>& mutations) {
        NekoGui_ConfigMutation::Guard mutationGuard(false);
        Result result;
        if (!mutationGuard.acquired()) {
            result.error = QStringLiteral(
                "Configuration transaction was refused while another model mutation is committing.");
            return result;
        }
        QMutexLocker processLock(&transactionMutex);
        if (!runtimeMutationBlockReason.isEmpty()) {
            result.outcome = Outcome::RecoveryRequired;
            result.error = runtimeMutationBlockReason;
            return result;
        }
        const auto normalizedOperation = operation.trimmed();
        if (normalizedOperation.isEmpty()) {
            result.error = QStringLiteral("Configuration transaction requires an operation label.");
            return result;
        }
        if (mutations.isEmpty()) {
            result.error = QStringLiteral("Configuration transaction requires at least one mutation.");
            return result;
        }

        DiskLockGuard diskLock;
        if (!diskLock.acquired()) {
            result.error = diskLock.error();
            return result;
        }
        const QDir transactionsRoot(transactionsRootPath());

        const auto existingIssues = scanTransactionIssues();
        if (!existingIssues.isEmpty()) {
            result.outcome = Outcome::RecoveryRequired;
            result.error = QStringLiteral("A previous configuration transaction requires recovery: %1")
                               .arg(existingIssues.join(QStringLiteral(" | ")));
            runtimeMutationBlockReason = result.error;
            return result;
        }

        QSet<QString> seenPaths;
        QStringList relativePaths;
        for (const auto& mutation: mutations) {
            QString relative;
            if (!relativeSourcePath(mutation.sourcePath, &relative, &result.error)) return result;
            const auto key = relative.toLower();
            if (seenPaths.contains(key)) {
                result.error = QStringLiteral("Configuration transaction contains duplicate target: %1")
                                   .arg(relative);
                return result;
            }
            if (statesEqual(mutation.before, mutation.after)) {
                result.error = QStringLiteral("Configuration transaction contains a no-op target: %1").arg(relative);
                return result;
            }
            if (!currentStateMatches(mutation.sourcePath, mutation.before, &result.error)) return result;
            seenPaths.insert(key);
            relativePaths.append(relative);
        }

        for (int index = 0; index < mutations.size(); ++index) {
            const auto& mutation = mutations.at(index);
            if (!mutation.before.exists) continue;

            NekoGui_ConfigRecovery::SnapshotResult evidence;
            if (mutation.after.exists) {
                evidence = NekoGui_ConfigRecovery::CreateBackupBeforeOverwrite(
                    mutation.sourcePath, mutation.before.content);
            } else {
                evidence = NekoGui_ConfigRecovery::RecordPreDeletion(
                    mutation.sourcePath, mutation.before.content, normalizedOperation);
            }
            if (!evidence.succeeded) {
                result.error = QStringLiteral("Cannot prepare recovery evidence for %1: %2")
                                   .arg(relativePaths.at(index), evidence.error);
                return result;
            }
        }

        const auto transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto stagingName = QStringLiteral(".staging-%1").arg(transactionId);
        const auto stagingPath = transactionsRoot.absoluteFilePath(stagingName);
        if (!NekoGui_ConfigPathSafety::EnsureDirectoryWithinRoot(
                transactionsRootPath(), stagingPath, &result.error)) {
            result.error = QStringLiteral("Cannot create safe transaction staging directory %1: %2")
                               .arg(stagingPath, result.error);
            return result;
        }
        auto abandonStaging = [&] {
            QString cleanupError;
            if (!removeUnpublishedStagingDirectory(stagingPath, &cleanupError)) {
                result.error += QStringLiteral("; staging cleanup failed: %1").arg(cleanupError);
            }
        };

        QJsonArray entries;
        for (int index = 0; index < mutations.size(); ++index) {
            const auto& mutation = mutations.at(index);
            const auto entryPrefix = QStringLiteral("%1").arg(index, 4, 10, QLatin1Char('0'));
            QString beforeSnapshot;
            QString afterSnapshot;
            if (mutation.before.exists) {
                beforeSnapshot = QStringLiteral("before/%1.bin").arg(entryPrefix);
                if (!writeAtomicExact(
                        QDir(stagingPath).absoluteFilePath(beforeSnapshot), mutation.before.content, &result.error)) {
                    abandonStaging();
                    return result;
                }
            }
            if (mutation.after.exists) {
                afterSnapshot = QStringLiteral("after/%1.bin").arg(entryPrefix);
                if (!writeAtomicExact(
                        QDir(stagingPath).absoluteFilePath(afterSnapshot), mutation.after.content, &result.error)) {
                    abandonStaging();
                    return result;
                }
            }

            QJsonObject entry;
            entry.insert(QStringLiteral("path"), relativePaths.at(index));
            entry.insert(QStringLiteral("before"), stateJson(mutation.before, beforeSnapshot));
            entry.insert(QStringLiteral("after"), stateJson(mutation.after, afterSnapshot));
            entries.append(entry);
        }

        auto currentManifest = manifestBytes(
            transactionId, normalizedOperation, QStringLiteral("prepared"), entries);
        const auto stagingManifestPath = QDir(stagingPath).absoluteFilePath(QStringLiteral("manifest.json"));
        if (!writeAtomicExact(stagingManifestPath, currentManifest, &result.error)) {
            abandonStaging();
            return result;
        }

        if (!QDir(transactionsRoot.absolutePath()).rename(stagingName, transactionId)) {
            result.error = QStringLiteral("Cannot publish prepared transaction directory: %1").arg(stagingPath);
            abandonStaging();
            return result;
        }

        result.transactionPath = transactionsRoot.absoluteFilePath(transactionId);
        const auto manifestPath = QDir(result.transactionPath).absoluteFilePath(QStringLiteral("manifest.json"));

        for (const auto& mutation: mutations) {
            if (currentStateMatches(mutation.sourcePath, mutation.before, &result.error)) continue;

            QString manifestError;
            if (!updateManifestState(
                    manifestPath,
                    &currentManifest,
                    transactionId,
                    normalizedOperation,
                    QStringLiteral("aborted"),
                    entries,
                    result.error,
                    &manifestError)) {
                result.outcome = Outcome::RecoveryRequired;
                result.error += QStringLiteral("; transaction abort marker failed: %1").arg(manifestError);
                runtimeMutationBlockReason = result.error;
            }
            return result;
        }

        QList<int> applied;
        QString applyError;
        for (int index = 0; index < mutations.size(); ++index) {
#ifdef NEKORAY_CONFIG_TRANSACTION_TESTING
            if (applyFailureAfter >= 0 && applied.size() >= applyFailureAfter) {
                applyError = QStringLiteral("Simulated transaction apply failure.");
                break;
            }
#endif
            const auto& mutation = mutations.at(index);
            bool mutationMayHaveApplied = false;
            if (!applyState(
                    mutation.sourcePath,
                    mutation.before,
                    mutation.after,
                    &mutationMayHaveApplied,
                    &applyError)) {
                if (mutationMayHaveApplied) applied.append(index);
                break;
            }
            applied.append(index);
        }

        if (applied.size() == mutations.size()) {
            QStringList verificationFailures;
            for (const auto& mutation: mutations) {
                QString verificationError;
                if (!currentStateMatches(
                        mutation.sourcePath, mutation.after, &verificationError)) {
                    verificationFailures.append(verificationError);
                }
            }
            if (verificationFailures.isEmpty()) {
                QString commitError;
                if (updateManifestState(
                        manifestPath,
                        &currentManifest,
                        transactionId,
                        normalizedOperation,
                        QStringLiteral("committed"),
                        entries,
                        {},
                        &commitError)) {
                    result.outcome = Outcome::Committed;
                    return result;
                }
                applyError = QStringLiteral("All mutations were written but the commit marker failed: %1")
                                 .arg(commitError);
            } else {
                applyError = QStringLiteral("Transaction final verification failed: %1")
                                 .arg(verificationFailures.join(QStringLiteral("; ")));
            }
        }

        if (applied.isEmpty()) {
            QString abortError;
            if (updateManifestState(
                    manifestPath,
                    &currentManifest,
                    transactionId,
                    normalizedOperation,
                    QStringLiteral("aborted"),
                    entries,
                    applyError,
                    &abortError)) {
                result.error = applyError;
                return result;
            }
            result.outcome = Outcome::RecoveryRequired;
            result.error = QStringLiteral("%1; transaction abort marker failed: %2").arg(applyError, abortError);
            runtimeMutationBlockReason = result.error;
            return result;
        }

        QString rollbackError;
        if (!rollbackApplied(mutations, applied, &rollbackError)) {
            QString markerError;
            updateManifestState(
                manifestPath,
                &currentManifest,
                transactionId,
                normalizedOperation,
                QStringLiteral("rollback_failed"),
                entries,
                rollbackError,
                &markerError);
            result.outcome = Outcome::RecoveryRequired;
            result.error = QStringLiteral("%1; rollback failed: %2").arg(applyError, rollbackError);
            if (!markerError.isEmpty()) {
                result.error += QStringLiteral("; rollback failure marker failed: %1").arg(markerError);
            }
            runtimeMutationBlockReason = result.error;
            return result;
        }

        QString markerError;
        if (!updateManifestState(
                manifestPath,
                &currentManifest,
                transactionId,
                normalizedOperation,
                QStringLiteral("rolled_back"),
                entries,
                applyError,
                &markerError)) {
            result.outcome = Outcome::RecoveryRequired;
            result.error = QStringLiteral("%1; disk rollback succeeded but its marker failed: %2")
                               .arg(applyError, markerError);
            runtimeMutationBlockReason = result.error;
            return result;
        }

        result.outcome = Outcome::RolledBack;
        result.error = applyError;
        return result;
    }

#ifdef NEKORAY_CONFIG_TRANSACTION_TESTING
    void SetApplyFailureAfterForTest(int appliedMutationCount) {
        applyFailureAfter = appliedMutationCount;
    }
#endif
} // namespace NekoGui_ConfigTransaction
