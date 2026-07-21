#include "ConfigTransaction.hpp"

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
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace {
    QString runtimeMutationBlockReason;
    QMutex transactionMutex;

#ifdef NEKORAY_CONFIG_TRANSACTION_TESTING
    int applyFailureAfter = -1;
#endif

    QByteArray sha256(const QByteArray &content) {
        return QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
    }

    QString transactionsRootPath() {
        return QDir(NekoGui_ConfigRecovery::RecoveryRootPath())
            .absoluteFilePath(QStringLiteral("transactions"));
    }

    bool relativeSourcePath(const QString &sourcePath, QString *relativePath, QString *error) {
        const QDir configRoot(QDir::currentPath());
        const auto absoluteSource = QFileInfo(sourcePath).absoluteFilePath();
        auto relative = QDir::cleanPath(configRoot.relativeFilePath(absoluteSource));
        relative.replace('\\', '/');

        if (relative.isEmpty() || relative == "." || relative == ".." ||
            relative.startsWith("../") || QDir::isAbsolutePath(relative)) {
            *error = QStringLiteral("Transaction refuses a source outside the active config directory: %1")
                         .arg(absoluteSource);
            return false;
        }
        if (relative == "recovery" || relative.startsWith("recovery/")) {
            *error = QStringLiteral("Transaction refuses to mutate recovery evidence: %1").arg(relative);
            return false;
        }

        *relativePath = relative;
        return true;
    }

    bool readExact(const QString &path, QByteArray *content, QString *error) {
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

    bool readState(const QString &path, NekoGui_ConfigTransaction::FileState *state, QString *error) {
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
        const NekoGui_ConfigTransaction::FileState &left,
        const NekoGui_ConfigTransaction::FileState &right) {
        return left.exists == right.exists && (!left.exists || left.content == right.content);
    }

    bool currentStateMatches(
        const QString &path,
        const NekoGui_ConfigTransaction::FileState &expected,
        QString *error) {
        NekoGui_ConfigTransaction::FileState current;
        if (!readState(path, &current, error)) return false;
        if (statesEqual(current, expected)) return true;

        *error = QStringLiteral("Transaction target changed outside the prepared state: %1").arg(path);
        return false;
    }

    bool writeAtomicExact(const QString &path, const QByteArray &content, QString *error) {
        const QFileInfo info(path);
        QDir parent;
        if (!parent.mkpath(info.absolutePath())) {
            *error = QStringLiteral("Cannot create transaction directory: %1").arg(info.absolutePath());
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
        const QString &path,
        const QByteArray &expected,
        const QByteArray &replacement,
        QString *error) {
        QByteArray current;
        if (!readExact(path, &current, error)) return false;
        if (current != expected) {
            *error = QStringLiteral("Transaction manifest changed before state update: %1").arg(path);
            return false;
        }
        return writeAtomicExact(path, replacement, error);
    }

    bool writeTargetAtomicExact(
        const QString &path,
        const NekoGui_ConfigTransaction::FileState &expected,
        const QByteArray &content,
        bool *committed,
        QString *error) {
        *committed = false;
        const QFileInfo info(path);
        QDir parent;
        if (!parent.mkpath(info.absolutePath())) {
            *error = QStringLiteral("Cannot create transaction target directory: %1").arg(info.absolutePath());
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
        const NekoGui_ConfigTransaction::FileState &state,
        const QString &snapshotPath) {
        QJsonObject object;
        object.insert(QStringLiteral("exists"), state.exists);
        if (state.exists) {
            object.insert(QStringLiteral("sha256"), QString::fromLatin1(sha256(state.content)));
            object.insert(QStringLiteral("size"), static_cast<double>(state.content.size()));
            object.insert(QStringLiteral("snapshot"), snapshotPath);
        }
        return object;
    }

    QByteArray manifestBytes(
        const QString &transactionId,
        const QString &operation,
        const QString &state,
        const QJsonArray &entries,
        const QString &detail = {}) {
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
        const QString &path,
        const NekoGui_ConfigTransaction::FileState &expected,
        const NekoGui_ConfigTransaction::FileState &desired,
        bool *mutationMayHaveApplied,
        QString *error) {
        *mutationMayHaveApplied = false;
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
        const QList<NekoGui_ConfigTransaction::FileMutation> &mutations,
        const QList<int> &applied,
        QString *error) {
        QStringList failures;
        for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
            const auto &mutation = mutations.at(*it);
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
        const QString &manifestPath,
        QByteArray *currentManifest,
        const QString &transactionId,
        const QString &operation,
        const QString &state,
        const QJsonArray &entries,
        const QString &detail,
        QString *error) {
        const auto replacement = manifestBytes(transactionId, operation, state, entries, detail);
        if (!replaceAtomicExact(manifestPath, *currentManifest, replacement, error)) return false;
        *currentManifest = replacement;
        return true;
    }

    QStringList scanTransactionIssues(bool includeActiveLock) {
        QStringList issues;
        const QDir root(transactionsRootPath());
        if (!root.exists()) return issues;

        if (includeActiveLock) {
            QLockFile lockProbe(root.absoluteFilePath(QStringLiteral("active.lock")));
            if (!lockProbe.tryLock(0)) {
                issues.append(QStringLiteral("%1: another process owns the active transaction lock")
                                  .arg(lockProbe.fileName()));
            } else {
                lockProbe.unlock();
            }
        }

        const auto directories = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto &directory: directories) {
            if (directory.fileName().startsWith(QStringLiteral(".staging-"))) continue;

            const auto manifestPath = QDir(directory.absoluteFilePath()).absoluteFilePath(QStringLiteral("manifest.json"));
            QByteArray bytes;
            QString error;
            if (!readExact(manifestPath, &bytes, &error)) {
                issues.append(QStringLiteral("%1: %2").arg(directory.absoluteFilePath(), error));
                continue;
            }

            QJsonParseError parseError{};
            const auto document = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                issues.append(
                    QStringLiteral("%1: invalid transaction manifest: %2")
                        .arg(directory.absoluteFilePath(), parseError.errorString()));
                continue;
            }

            const auto manifest = document.object();
            const auto schema = manifest.value(QStringLiteral("schema")).toString();
            const auto id = manifest.value(QStringLiteral("id")).toString();
            const auto state = manifest.value(QStringLiteral("state")).toString();
            if (schema != QStringLiteral("nekoray.config_transaction.v1") || id != directory.fileName()) {
                issues.append(QStringLiteral("%1: transaction identity or schema mismatch")
                                  .arg(directory.absoluteFilePath()));
                continue;
            }
            if (state == "committed" || state == "rolled_back" || state == "aborted") continue;

            issues.append(
                QStringLiteral("%1: transaction state '%2' requires explicit recovery")
                    .arg(directory.absoluteFilePath(), state.isEmpty() ? QStringLiteral("missing") : state));
        }
        return issues;
    }
}

namespace NekoGui_ConfigTransaction {
    QString RuntimeMutationBlockReason() {
        QMutexLocker lock(&transactionMutex);
        return runtimeMutationBlockReason;
    }

    QStringList BlockingTransactionIssues() {
        return scanTransactionIssues(true);
    }

    Result Execute(const QString &operation, const QList<FileMutation> &mutations) {
        QMutexLocker processLock(&transactionMutex);
        Result result;
        const auto normalizedOperation = operation.trimmed();
        if (normalizedOperation.isEmpty()) {
            result.error = QStringLiteral("Configuration transaction requires an operation label.");
            return result;
        }
        if (mutations.isEmpty()) {
            result.error = QStringLiteral("Configuration transaction requires at least one mutation.");
            return result;
        }

        const QDir transactionsRoot(transactionsRootPath());
        if (!QDir().mkpath(transactionsRoot.absolutePath())) {
            result.error = QStringLiteral("Cannot create transaction root: %1").arg(transactionsRoot.absolutePath());
            return result;
        }
        QLockFile fileLock(transactionsRoot.absoluteFilePath(QStringLiteral("active.lock")));
        if (!fileLock.tryLock(0)) {
            result.error = QStringLiteral("Another process owns the configuration transaction lock: %1")
                               .arg(fileLock.fileName());
            return result;
        }

        const auto existingIssues = scanTransactionIssues(false);
        if (!existingIssues.isEmpty()) {
            result.outcome = Outcome::RecoveryRequired;
            result.error = QStringLiteral("A previous configuration transaction requires recovery: %1")
                               .arg(existingIssues.join(QStringLiteral(" | ")));
            runtimeMutationBlockReason = result.error;
            return result;
        }

        QSet<QString> seenPaths;
        QStringList relativePaths;
        for (const auto &mutation: mutations) {
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
            const auto &mutation = mutations.at(index);
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
        if (!QDir().mkpath(stagingPath)) {
            result.error = QStringLiteral("Cannot create transaction staging directory: %1").arg(stagingPath);
            return result;
        }

        QJsonArray entries;
        for (int index = 0; index < mutations.size(); ++index) {
            const auto &mutation = mutations.at(index);
            const auto entryPrefix = QStringLiteral("%1").arg(index, 4, 10, QLatin1Char('0'));
            QString beforeSnapshot;
            QString afterSnapshot;
            if (mutation.before.exists) {
                beforeSnapshot = QStringLiteral("before/%1.bin").arg(entryPrefix);
                if (!writeAtomicExact(
                        QDir(stagingPath).absoluteFilePath(beforeSnapshot), mutation.before.content, &result.error)) {
                    return result;
                }
            }
            if (mutation.after.exists) {
                afterSnapshot = QStringLiteral("after/%1.bin").arg(entryPrefix);
                if (!writeAtomicExact(
                        QDir(stagingPath).absoluteFilePath(afterSnapshot), mutation.after.content, &result.error)) {
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
        if (!writeAtomicExact(stagingManifestPath, currentManifest, &result.error)) return result;

        if (!QDir(transactionsRoot.absolutePath()).rename(stagingName, transactionId)) {
            result.error = QStringLiteral("Cannot publish prepared transaction directory: %1").arg(stagingPath);
            return result;
        }

        result.transactionPath = transactionsRoot.absoluteFilePath(transactionId);
        const auto manifestPath = QDir(result.transactionPath).absoluteFilePath(QStringLiteral("manifest.json"));

        for (const auto &mutation: mutations) {
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
            const auto &mutation = mutations.at(index);
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
}
