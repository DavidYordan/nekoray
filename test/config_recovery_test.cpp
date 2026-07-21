#include "main/ConfigRecovery.hpp"
#include "main/ConfigTransaction.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
    bool writeFile(const QString& path, const QByteArray& content) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        return file.write(content) == content.size();
    }

    QByteArray readFile(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }

    bool require(bool condition, const QString& message) {
        if (condition) return true;
        QTextStream(stderr) << message << Qt::endl;
        return false;
    }
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir lab;
    if (!require(lab.isValid(), QStringLiteral("temporary directory creation failed"))) return 1;

    QDir labDir(lab.path());
    if (!require(labDir.mkpath(QStringLiteral("config/groups")), QStringLiteral("config directory creation failed"))) {
        return 1;
    }
    if (!require(QDir::setCurrent(labDir.absoluteFilePath(QStringLiteral("config"))),
                 QStringLiteral("cannot enter config directory"))) {
        return 1;
    }

    const QString sourcePath = QStringLiteral("groups/7.json");
    const QByteArray sourceContent = R"({"id":7,"name":"original"})";
    if (!require(writeFile(sourcePath, sourceContent), QStringLiteral("cannot create source file"))) return 1;

    const auto firstBackup = NekoGui_ConfigRecovery::CreateBackupBeforeOverwrite(sourcePath, sourceContent);
    if (!require(firstBackup.succeeded, firstBackup.error) ||
        !require(readFile(firstBackup.snapshotPath) == sourceContent, QStringLiteral("backup content mismatch"))) {
        return 1;
    }

    const auto repeatedBackup = NekoGui_ConfigRecovery::CreateBackupBeforeOverwrite(sourcePath, sourceContent);
    if (!require(repeatedBackup.succeeded, repeatedBackup.error) ||
        !require(repeatedBackup.snapshotPath == firstBackup.snapshotPath,
                 QStringLiteral("content-addressed backup path changed"))) {
        return 1;
    }

    const QByteArray updatedContent = R"({"id":7,"name":"updated"})";
    const auto overwrite = NekoGui_ConfigRecovery::PrepareOverwrite(
        sourcePath, sourceContent, updatedContent);
    if (!require(overwrite.decision == NekoGui_ConfigRecovery::OverwriteDecision::Proceed,
                 overwrite.error) ||
        !require(overwrite.targetExisted, QStringLiteral("existing overwrite target was not detected")) ||
        !require(overwrite.backupPath == firstBackup.snapshotPath,
                 QStringLiteral("overwrite did not use the verified content-addressed backup"))) {
        return 1;
    }

    const auto unchanged = NekoGui_ConfigRecovery::PrepareOverwrite(
        sourcePath, sourceContent, sourceContent);
    if (!require(unchanged.decision == NekoGui_ConfigRecovery::OverwriteDecision::Unchanged,
                 QStringLiteral("unchanged config was not recognized"))) {
        return 1;
    }

    if (!require(writeFile(sourcePath, QByteArrayLiteral("externally changed")),
                 QStringLiteral("cannot create external-change fixture"))) {
        return 1;
    }
    const auto externalChange = NekoGui_ConfigRecovery::PrepareOverwrite(
        sourcePath, sourceContent, updatedContent);
    if (!require(externalChange.decision == NekoGui_ConfigRecovery::OverwriteDecision::Refused,
                 QStringLiteral("external config change was not rejected"))) {
        return 1;
    }
    if (!require(writeFile(sourcePath, sourceContent), QStringLiteral("cannot restore source fixture"))) return 1;

    const auto firstQuarantine = NekoGui_ConfigRecovery::RecordQuarantine(
        sourcePath, sourceContent, QStringLiteral("first reason"));
    const auto secondQuarantine = NekoGui_ConfigRecovery::RecordQuarantine(
        sourcePath, sourceContent, QStringLiteral("second reason"));
    if (!require(firstQuarantine.succeeded, firstQuarantine.error) ||
        !require(secondQuarantine.succeeded, secondQuarantine.error) ||
        !require(firstQuarantine.snapshotPath == secondQuarantine.snapshotPath,
                 QStringLiteral("quarantine path changed for identical content"))) {
        return 1;
    }

    const auto metadataBytes = readFile(firstQuarantine.snapshotPath + QStringLiteral(".meta.json"));
    const auto metadata = QJsonDocument::fromJson(metadataBytes).object();
    if (!require(metadata.value(QStringLiteral("schema")).toString() == "nekoray.recovery.quarantine.v1",
                 QStringLiteral("quarantine schema mismatch")) ||
        !require(metadata.value(QStringLiteral("source_path")).toString() == sourcePath,
                 QStringLiteral("quarantine source path mismatch")) ||
        !require(metadata.value(QStringLiteral("reasons")).toArray().size() == 2,
                 QStringLiteral("quarantine reasons were not merged"))) {
        return 1;
    }

    const auto deletion = NekoGui_ConfigRecovery::PrepareDeletion(
        sourcePath, sourceContent, QStringLiteral("explicit test deletion"));
    if (!require(deletion.ready, deletion.error) ||
        !require(readFile(deletion.snapshotPath) == sourceContent,
                 QStringLiteral("deletion snapshot content mismatch")) ||
        !require(readFile(sourcePath) == sourceContent,
                 QStringLiteral("preparation unexpectedly removed the source"))) {
        return 1;
    }
    const auto deletionMetadata = QJsonDocument::fromJson(
                                      readFile(deletion.snapshotPath + QStringLiteral(".meta.json")))
                                      .object();
    if (!require(deletionMetadata.value(QStringLiteral("schema")).toString() ==
                     "nekoray.recovery.pre_delete.v1",
                 QStringLiteral("deletion schema mismatch")) ||
        !require(!deletionMetadata.value(QStringLiteral("first_prepared_utc")).toString().isEmpty(),
                 QStringLiteral("deletion preparation time missing")) ||
        !require(deletionMetadata.value(QStringLiteral("reasons")).toArray().first().toString() ==
                     "explicit test deletion",
                 QStringLiteral("deletion reason mismatch"))) {
        return 1;
    }

    const auto repeatedDeletion = NekoGui_ConfigRecovery::PrepareDeletion(
        sourcePath, sourceContent, QStringLiteral("second deletion reason"));
    const auto repeatedDeletionMetadata = QJsonDocument::fromJson(
                                              readFile(deletion.snapshotPath + QStringLiteral(".meta.json")))
                                              .object();
    if (!require(repeatedDeletion.ready, repeatedDeletion.error) ||
        !require(repeatedDeletion.snapshotPath == deletion.snapshotPath,
                 QStringLiteral("content-addressed deletion path changed")) ||
        !require(repeatedDeletionMetadata.value(QStringLiteral("reasons")).toArray().size() == 2,
                 QStringLiteral("deletion reasons were not merged"))) {
        return 1;
    }

    const auto missingDeletion = NekoGui_ConfigRecovery::PrepareDeletion(
        QStringLiteral("groups/missing.json"), sourceContent, QStringLiteral("must be refused"));
    if (!require(!missingDeletion.ready, QStringLiteral("missing deletion source was accepted"))) return 1;

    if (!require(writeFile(sourcePath, QByteArrayLiteral("changed before deletion")),
                 QStringLiteral("cannot create deletion race fixture"))) {
        return 1;
    }
    const auto deletionRace = NekoGui_ConfigRecovery::PrepareDeletion(
        sourcePath, sourceContent, QStringLiteral("must be refused"));
    if (!require(!deletionRace.ready, QStringLiteral("externally changed deletion source was accepted"))) {
        return 1;
    }
    if (!require(writeFile(sourcePath, sourceContent), QStringLiteral("cannot restore deletion fixture"))) return 1;

    const auto outside = NekoGui_ConfigRecovery::CreateBackupBeforeOverwrite(
        labDir.absoluteFilePath(QStringLiteral("outside.json")), sourceContent);
    if (!require(!outside.succeeded, QStringLiteral("outside-config snapshot was not rejected"))) return 1;

    if (!require(writeFile(firstBackup.snapshotPath, QByteArrayLiteral("tampered")),
                 QStringLiteral("cannot create tampered backup fixture"))) {
        return 1;
    }
    const auto collision = NekoGui_ConfigRecovery::CreateBackupBeforeOverwrite(sourcePath, sourceContent);
    if (!require(!collision.succeeded, QStringLiteral("tampered existing backup was accepted"))) return 1;

    const auto replacePath = QStringLiteral("groups/20.json");
    const auto deletePath = QStringLiteral("groups/21.json");
    const auto createPath = QStringLiteral("groups/22.json");
    const QByteArray replaceBefore = R"({"id":20,"name":"before"})";
    const QByteArray replaceAfter = R"({"id":20,"name":"after"})";
    const QByteArray deleteBefore = R"({"id":21,"name":"delete"})";
    const QByteArray createAfter = R"({"id":22,"name":"create"})";
    if (!require(writeFile(replacePath, replaceBefore), QStringLiteral("cannot create replace fixture")) ||
        !require(writeFile(deletePath, deleteBefore), QStringLiteral("cannot create delete fixture"))) {
        return 1;
    }

    const auto committedTransaction = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("successful test transaction"),
        {
            {replacePath, {true, replaceBefore}, {true, replaceAfter}},
            {deletePath, {true, deleteBefore}, {false, {}}},
            {createPath, {false, {}}, {true, createAfter}},
        });
    if (!require(committedTransaction.succeeded(), committedTransaction.error) ||
        !require(readFile(replacePath) == replaceAfter, QStringLiteral("transaction replacement mismatch")) ||
        !require(!QFileInfo::exists(deletePath), QStringLiteral("transaction deletion did not occur")) ||
        !require(readFile(createPath) == createAfter, QStringLiteral("transaction creation mismatch")) ||
        !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                 QStringLiteral("committed transaction was reported as blocking"))) {
        return 1;
    }
    const auto committedManifest = QJsonDocument::fromJson(
                                       readFile(QDir(committedTransaction.transactionPath).absoluteFilePath(QStringLiteral("manifest.json"))))
                                       .object();
    if (!require(committedManifest.value(QStringLiteral("state")).toString() == "committed",
                 QStringLiteral("transaction commit marker missing")) ||
        !require(committedManifest.value(QStringLiteral("entries")).toArray().size() == 3,
                 QStringLiteral("transaction manifest entries missing"))) {
        return 1;
    }

    const auto rollbackReplacePath = QStringLiteral("groups/30.json");
    const auto rollbackDeletePath = QStringLiteral("groups/31.json");
    const QByteArray rollbackReplaceBefore = R"({"id":30,"name":"before"})";
    const QByteArray rollbackReplaceAfter = R"({"id":30,"name":"after"})";
    const QByteArray rollbackDeleteBefore = R"({"id":31,"name":"preserve"})";
    if (!require(writeFile(rollbackReplacePath, rollbackReplaceBefore),
                 QStringLiteral("cannot create rollback replace fixture")) ||
        !require(writeFile(rollbackDeletePath, rollbackDeleteBefore),
                 QStringLiteral("cannot create rollback delete fixture"))) {
        return 1;
    }

    NekoGui_ConfigTransaction::SetApplyFailureAfterForTest(1);
    const auto rolledBackTransaction = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("rollback test transaction"),
        {
            {rollbackReplacePath, {true, rollbackReplaceBefore}, {true, rollbackReplaceAfter}},
            {rollbackDeletePath, {true, rollbackDeleteBefore}, {false, {}}},
        });
    NekoGui_ConfigTransaction::SetApplyFailureAfterForTest(-1);
    if (!require(rolledBackTransaction.outcome == NekoGui_ConfigTransaction::Outcome::RolledBack,
                 rolledBackTransaction.error) ||
        !require(readFile(rollbackReplacePath) == rollbackReplaceBefore,
                 QStringLiteral("transaction rollback did not restore replacement")) ||
        !require(readFile(rollbackDeletePath) == rollbackDeleteBefore,
                 QStringLiteral("transaction rollback unexpectedly removed untouched file")) ||
        !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                 QStringLiteral("rolled-back transaction was reported as blocking"))) {
        return 1;
    }

    const auto lockedPath = QStringLiteral("groups/40.json");
    const QByteArray lockedBefore = R"({"id":40,"name":"locked"})";
    const QByteArray lockedAfter = R"({"id":40,"name":"must-not-change"})";
    if (!require(writeFile(lockedPath, lockedBefore), QStringLiteral("cannot create lock fixture"))) return 1;
    QLockFile competingLock(
        QDir(NekoGui_ConfigRecovery::RecoveryRootPath())
            .absoluteFilePath(QStringLiteral("transactions/active.lock")));
    if (!require(competingLock.tryLock(0), QStringLiteral("cannot acquire competing transaction lock"))) return 1;
    const auto activeLockIssues = NekoGui_ConfigTransaction::BlockingTransactionIssues();
    const auto lockedTransaction = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("lock contention test"),
        {{lockedPath, {true, lockedBefore}, {true, lockedAfter}}});
    competingLock.unlock();
    if (!require(activeLockIssues.size() == 1, QStringLiteral("active transaction lock did not block startup")) ||
        !require(lockedTransaction.outcome == NekoGui_ConfigTransaction::Outcome::PreparationFailed,
                 QStringLiteral("competing transaction lock was ignored")) ||
        !require(readFile(lockedPath) == lockedBefore,
                 QStringLiteral("lock contention changed its target"))) {
        return 1;
    }

    const auto nestedLockPath = QStringLiteral("groups/41.json");
    const QByteArray nestedLockAfter = R"({"id":41,"name":"nested"})";
    {
        NekoGui_ConfigTransaction::DiskLockGuard startupLock;
        if (!require(startupLock.acquired(), startupLock.error()) ||
            !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                     QStringLiteral("nested startup lock was not process-reentrant"))) {
            return 1;
        }
        const auto nestedTransaction = NekoGui_ConfigTransaction::Execute(
            QStringLiteral("startup lock nesting test"),
            {{nestedLockPath, {false, {}}, {true, nestedLockAfter}}});
        if (!require(nestedTransaction.succeeded(), nestedTransaction.error) ||
            !require(readFile(nestedLockPath) == nestedLockAfter,
                     QStringLiteral("nested transaction did not commit"))) {
            return 1;
        }
    }

    const auto reservedRecoveryMutation = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("reserved recovery alias rejection test"),
        {{QStringLiteral("Recovery/forbidden.json"), {false, {}}, {true, QByteArray("forbidden")}}});
    if (!require(!reservedRecoveryMutation.succeeded(),
                 QStringLiteral("case-insensitive recovery alias was accepted")) ||
        !require(!QFileInfo::exists(QStringLiteral("Recovery/forbidden.json")),
                 QStringLiteral("reserved recovery alias was modified"))) {
        return 1;
    }

    const auto recoveryReplacePath = QStringLiteral("groups/50.json");
    const auto recoveryCreatePath = QStringLiteral("groups/51.json");
    const QByteArray recoveryReplaceBefore = R"({"id":50,"name":"before"})";
    const QByteArray recoveryReplaceAfter = R"({"id":50,"name":"after"})";
    const QByteArray recoveryCreateAfter = R"({"id":51,"name":"created"})";
    if (!require(writeFile(recoveryReplacePath, recoveryReplaceBefore),
                 QStringLiteral("cannot create manual recovery replacement fixture"))) {
        return 1;
    }
    const auto recoveryTransaction = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("manual rollback fixture"),
        {
            {recoveryReplacePath, {true, recoveryReplaceBefore}, {true, recoveryReplaceAfter}},
            {recoveryCreatePath, {false, {}}, {true, recoveryCreateAfter}},
        });
    if (!require(recoveryTransaction.succeeded(), recoveryTransaction.error)) return 1;
    const auto recoveryManifestPath =
        QDir(recoveryTransaction.transactionPath).absoluteFilePath(QStringLiteral("manifest.json"));
    auto recoveryManifest = QJsonDocument::fromJson(readFile(recoveryManifestPath)).object();
    recoveryManifest.insert(QStringLiteral("state"), QStringLiteral("prepared"));
    if (!require(writeFile(
                     recoveryManifestPath,
                     QJsonDocument(recoveryManifest).toJson(QJsonDocument::Indented)),
                 QStringLiteral("cannot simulate interrupted committed transaction"))) {
        return 1;
    }

    const auto recoveryReport = NekoGui_ConfigTransaction::BuildRecoveryReport();
    const auto recoveryReportTransactions =
        QJsonDocument::fromJson(recoveryReport.json).object().value(QStringLiteral("transactions")).toArray();
    if (!require(recoveryReport.succeeded, recoveryReport.error) ||
        !require(recoveryReportTransactions.size() == 1,
                 QStringLiteral("manual recovery report did not list one pending transaction")) ||
        !require(recoveryReportTransactions.first().toObject().value(QStringLiteral("recoverable")).toBool(),
                 QStringLiteral("valid pending transaction was not reported as recoverable"))) {
        return 1;
    }

    const auto manualRollback = NekoGui_ConfigTransaction::Recover(
        QFileInfo(recoveryTransaction.transactionPath).fileName(),
        NekoGui_ConfigTransaction::RecoveryDirection::RestoreBefore);
    if (!require(manualRollback.succeeded, manualRollback.error) ||
        !require(manualRollback.finalState == QStringLiteral("rolled_back"),
                 QStringLiteral("manual rollback final state mismatch")) ||
        !require(readFile(recoveryReplacePath) == recoveryReplaceBefore,
                 QStringLiteral("manual rollback did not restore replacement")) ||
        !require(!QFileInfo::exists(recoveryCreatePath),
                 QStringLiteral("manual rollback did not remove created target")) ||
        !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                 QStringLiteral("completed manual rollback still blocks startup"))) {
        return 1;
    }
    recoveryManifest = QJsonDocument::fromJson(readFile(recoveryManifestPath)).object();
    if (!require(recoveryManifest.value(QStringLiteral("recovery_direction")).toString() == "before",
                 QStringLiteral("manual rollback direction was not audited")) ||
        !require(!recoveryManifest.value(QStringLiteral("recovered_utc")).toString().isEmpty(),
                 QStringLiteral("manual rollback completion time is missing"))) {
        return 1;
    }

    const auto rollForwardReplacePath = QStringLiteral("groups/60.json");
    const auto rollForwardDeletePath = QStringLiteral("groups/61.json");
    const QByteArray rollForwardReplaceBefore = R"({"id":60,"name":"before"})";
    const QByteArray rollForwardReplaceAfter = R"({"id":60,"name":"after"})";
    const QByteArray rollForwardDeleteBefore = R"({"id":61,"name":"delete"})";
    if (!require(writeFile(rollForwardReplacePath, rollForwardReplaceBefore),
                 QStringLiteral("cannot create roll-forward replacement fixture")) ||
        !require(writeFile(rollForwardDeletePath, rollForwardDeleteBefore),
                 QStringLiteral("cannot create roll-forward deletion fixture"))) {
        return 1;
    }
    const auto rollForwardTransaction = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("manual roll-forward fixture"),
        {
            {rollForwardReplacePath,
             {true, rollForwardReplaceBefore},
             {true, rollForwardReplaceAfter}},
            {rollForwardDeletePath, {true, rollForwardDeleteBefore}, {false, {}}},
        });
    if (!require(rollForwardTransaction.succeeded(), rollForwardTransaction.error)) return 1;
    const auto rollForwardManifestPath =
        QDir(rollForwardTransaction.transactionPath).absoluteFilePath(QStringLiteral("manifest.json"));
    auto rollForwardManifest = QJsonDocument::fromJson(readFile(rollForwardManifestPath)).object();
    rollForwardManifest.insert(QStringLiteral("state"), QStringLiteral("prepared"));
    if (!require(writeFile(
                     rollForwardManifestPath,
                     QJsonDocument(rollForwardManifest).toJson(QJsonDocument::Indented)),
                 QStringLiteral("cannot prepare roll-forward manifest")) ||
        !require(writeFile(rollForwardReplacePath, rollForwardReplaceBefore),
                 QStringLiteral("cannot create mixed before/after recovery state"))) {
        return 1;
    }
    const auto manualRollForward = NekoGui_ConfigTransaction::Recover(
        QFileInfo(rollForwardTransaction.transactionPath).fileName(),
        NekoGui_ConfigTransaction::RecoveryDirection::ApplyAfter);
    if (!require(manualRollForward.succeeded, manualRollForward.error) ||
        !require(manualRollForward.finalState == QStringLiteral("committed"),
                 QStringLiteral("manual roll-forward final state mismatch")) ||
        !require(readFile(rollForwardReplacePath) == rollForwardReplaceAfter,
                 QStringLiteral("manual roll-forward did not apply replacement")) ||
        !require(!QFileInfo::exists(rollForwardDeletePath),
                 QStringLiteral("manual roll-forward did not preserve deletion"))) {
        return 1;
    }

    const auto lockedDirectionPath = QStringLiteral("groups/70.json");
    const QByteArray lockedDirectionBefore = R"({"id":70,"name":"before"})";
    const QByteArray lockedDirectionAfter = R"({"id":70,"name":"after"})";
    if (!require(writeFile(lockedDirectionPath, lockedDirectionBefore),
                 QStringLiteral("cannot create recovery direction fixture"))) {
        return 1;
    }
    const auto lockedDirectionTransaction = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("manual recovery direction lock fixture"),
        {{lockedDirectionPath, {true, lockedDirectionBefore}, {true, lockedDirectionAfter}}});
    if (!require(lockedDirectionTransaction.succeeded(), lockedDirectionTransaction.error)) return 1;
    const auto lockedDirectionManifestPath =
        QDir(lockedDirectionTransaction.transactionPath).absoluteFilePath(QStringLiteral("manifest.json"));
    auto lockedDirectionManifest = QJsonDocument::fromJson(readFile(lockedDirectionManifestPath)).object();
    lockedDirectionManifest.insert(QStringLiteral("state"), QStringLiteral("recovering_before"));
    lockedDirectionManifest.insert(QStringLiteral("recovery_direction"), QStringLiteral("after"));
    if (!require(writeFile(
                     lockedDirectionManifestPath,
                     QJsonDocument(lockedDirectionManifest).toJson(QJsonDocument::Indented)),
                 QStringLiteral("cannot create inconsistent recovery direction manifest"))) {
        return 1;
    }
    const auto rejectedInconsistentDirection = NekoGui_ConfigTransaction::Recover(
        QFileInfo(lockedDirectionTransaction.transactionPath).fileName(),
        NekoGui_ConfigTransaction::RecoveryDirection::RestoreBefore);
    const auto inconsistentDirectionReport = NekoGui_ConfigTransaction::BuildRecoveryReport();
    const auto inconsistentTransactions = QJsonDocument::fromJson(inconsistentDirectionReport.json)
                                              .object()
                                              .value(QStringLiteral("transactions"))
                                              .toArray();
    if (!require(!rejectedInconsistentDirection.succeeded,
                 QStringLiteral("inconsistent recovery direction metadata was accepted")) ||
        !require(inconsistentTransactions.size() == 1 &&
                     !inconsistentTransactions.first().toObject().value(QStringLiteral("recoverable")).toBool(),
                 QStringLiteral("inconsistent recovery direction was reported recoverable")) ||
        !require(readFile(lockedDirectionPath) == lockedDirectionAfter,
                 QStringLiteral("inconsistent direction check modified its target"))) {
        return 1;
    }

    lockedDirectionManifest.insert(QStringLiteral("state"), QStringLiteral("recovery_failed"));
    lockedDirectionManifest.insert(QStringLiteral("recovery_direction"), QStringLiteral("before"));
    if (!require(writeFile(
                     lockedDirectionManifestPath,
                     QJsonDocument(lockedDirectionManifest).toJson(QJsonDocument::Indented)),
                 QStringLiteral("cannot create locked recovery direction manifest"))) {
        return 1;
    }
    const auto rejectedDirectionChange = NekoGui_ConfigTransaction::Recover(
        QFileInfo(lockedDirectionTransaction.transactionPath).fileName(),
        NekoGui_ConfigTransaction::RecoveryDirection::ApplyAfter);
    if (!require(!rejectedDirectionChange.succeeded,
                 QStringLiteral("manual recovery direction change was accepted")) ||
        !require(readFile(lockedDirectionPath) == lockedDirectionAfter,
                 QStringLiteral("rejected direction change modified its target"))) {
        return 1;
    }
    const auto completedLockedRecovery = NekoGui_ConfigTransaction::Recover(
        QFileInfo(lockedDirectionTransaction.transactionPath).fileName(),
        NekoGui_ConfigTransaction::RecoveryDirection::RestoreBefore);
    if (!require(completedLockedRecovery.succeeded, completedLockedRecovery.error) ||
        !require(readFile(lockedDirectionPath) == lockedDirectionBefore,
                 QStringLiteral("locked recovery did not restore before state"))) {
        return 1;
    }

    {
        NekoGui_ConfigTransaction::DiskLockGuard intentDiskLock;
        if (!require(intentDiskLock.acquired(), intentDiskLock.error())) return 1;
        const auto indeterminatePath = QStringLiteral("groups/80.json");
        const QByteArray indeterminateBefore = R"({"id":80,"name":"before"})";
        const QByteArray indeterminateAfter = R"({"id":80,"name":"after"})";
        if (!require(writeFile(indeterminatePath, indeterminateBefore),
                     QStringLiteral("cannot create indeterminate-save target"))) {
            return 1;
        }
        const auto indeterminateIntent = NekoGui_ConfigTransaction::PrepareMutationIntent(
            QStringLiteral("indeterminate atomic save fixture"),
            {indeterminatePath,
             {true, indeterminateBefore},
             {true, indeterminateAfter}});
        if (!require(indeterminateIntent.succeeded, indeterminateIntent.error) ||
            !require(writeFile(indeterminatePath, indeterminateAfter),
                     QStringLiteral("cannot simulate committed indeterminate save"))) {
            return 1;
        }
        const auto indeterminateCompletion = NekoGui_ConfigTransaction::CompleteMutationIntent(
            indeterminateIntent.transactionId,
            NekoGui_ConfigTransaction::MutationIntentDisposition::Indeterminate,
            QStringLiteral("simulated post-commit verification failure"));
        const auto indeterminateId = indeterminateIntent.transactionId;
        const auto wrongCaseId = indeterminateId.toUpper();
        if (!require(!indeterminateCompletion.resolved,
                     QStringLiteral("indeterminate intent was incorrectly resolved")) ||
            !require(!NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                     QStringLiteral("indeterminate save marker did not block mutations")) ||
            !require(wrongCaseId != indeterminateId,
                     QStringLiteral("indeterminate transaction id has no case-test character"))) {
            return 1;
        }
        const auto rejectedCaseAlias = NekoGui_ConfigTransaction::Recover(
            wrongCaseId,
            NekoGui_ConfigTransaction::RecoveryDirection::RestoreBefore);
        if (!require(!rejectedCaseAlias.succeeded,
                     QStringLiteral("case-mismatched recovery id was accepted")) ||
            !require(readFile(indeterminatePath) == indeterminateAfter,
                     QStringLiteral("case-mismatched recovery modified its target"))) {
            return 1;
        }
        const auto recoveredIndeterminate = NekoGui_ConfigTransaction::Recover(
            indeterminateId,
            NekoGui_ConfigTransaction::RecoveryDirection::RestoreBefore);
        if (!require(recoveredIndeterminate.succeeded, recoveredIndeterminate.error) ||
            !require(readFile(indeterminatePath) == indeterminateBefore,
                     QStringLiteral("indeterminate save recovery did not restore before")) ||
            !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                     QStringLiteral("recovered indeterminate save still blocks mutations"))) {
            return 1;
        }
    }

    {
        NekoGui_ConfigTransaction::DiskLockGuard intentDiskLock;
        if (!require(intentDiskLock.acquired(), intentDiskLock.error())) return 1;
        const auto completedIntentPath = QStringLiteral("groups/81.json");
        const QByteArray completedIntentBefore = R"({"id":81,"name":"before"})";
        const QByteArray completedIntentAfter = R"({"id":81,"name":"after"})";
        if (!require(writeFile(completedIntentPath, completedIntentBefore),
                     QStringLiteral("cannot create completed intent target"))) {
            return 1;
        }
        const auto completedIntent = NekoGui_ConfigTransaction::PrepareMutationIntent(
            QStringLiteral("verified single-file intent fixture"),
            {completedIntentPath,
             {true, completedIntentBefore},
             {true, completedIntentAfter}});
        if (!require(completedIntent.succeeded, completedIntent.error) ||
            !require(writeFile(completedIntentPath, completedIntentAfter),
                     QStringLiteral("cannot apply completed intent fixture"))) {
            return 1;
        }
        const auto completedIntentResult = NekoGui_ConfigTransaction::CompleteMutationIntent(
            completedIntent.transactionId,
            NekoGui_ConfigTransaction::MutationIntentDisposition::VerifiedAfter,
            QStringLiteral("verified intent completion fixture"));
        if (!require(completedIntentResult.resolved, completedIntentResult.error) ||
            !require(completedIntentResult.finalState == QStringLiteral("committed"),
                     QStringLiteral("verified intent did not record committed state")) ||
            !require(readFile(completedIntentPath) == completedIntentAfter,
                     QStringLiteral("verified intent target changed")) ||
            !require(!QFileInfo::exists(completedIntent.transactionPath),
                     QStringLiteral("terminal single-file intent was not retired")) ||
            !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                     QStringLiteral("completed single-file intent still blocks startup"))) {
            return 1;
        }
    }

    {
        NekoGui_ConfigTransaction::DiskLockGuard intentDiskLock;
        if (!require(intentDiskLock.acquired(), intentDiskLock.error())) return 1;
        const auto abortedIntentPath = QStringLiteral("groups/82.json");
        const QByteArray abortedIntentBefore = R"({"id":82,"name":"before"})";
        const QByteArray abortedIntentAfter = R"({"id":82,"name":"after"})";
        if (!require(writeFile(abortedIntentPath, abortedIntentBefore),
                     QStringLiteral("cannot create aborted intent target"))) {
            return 1;
        }
        const auto abortedIntent = NekoGui_ConfigTransaction::PrepareMutationIntent(
            QStringLiteral("verified-before single-file intent fixture"),
            {abortedIntentPath,
             {true, abortedIntentBefore},
             {true, abortedIntentAfter}});
        if (!require(abortedIntent.succeeded, abortedIntent.error)) return 1;
        const auto abortedIntentResult = NekoGui_ConfigTransaction::CompleteMutationIntent(
            abortedIntent.transactionId,
            NekoGui_ConfigTransaction::MutationIntentDisposition::VerifiedBefore,
            QStringLiteral("simulated target-open failure"));
        if (!require(abortedIntentResult.resolved, abortedIntentResult.error) ||
            !require(abortedIntentResult.finalState == QStringLiteral("aborted"),
                     QStringLiteral("verified-before intent did not record aborted state")) ||
            !require(readFile(abortedIntentPath) == abortedIntentBefore,
                     QStringLiteral("aborted intent target changed")) ||
            !require(!QFileInfo::exists(abortedIntent.transactionPath),
                     QStringLiteral("aborted single-file intent was not retired")) ||
            !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                     QStringLiteral("aborted single-file intent still blocks startup"))) {
            return 1;
        }
    }

    const auto transactionsRoot = QDir(NekoGui_ConfigRecovery::RecoveryRootPath())
                                      .absoluteFilePath(QStringLiteral("transactions"));
    const auto stagingDirectory = QDir(transactionsRoot).absoluteFilePath(QStringLiteral(".staging-11111111-1111-1111-1111-111111111111"));
    if (!require(QDir().mkpath(stagingDirectory),
                 QStringLiteral("cannot create unpublished staging fixture")) ||
        !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().isEmpty(),
                 QStringLiteral("protocol staging directory incorrectly blocked startup")) ||
        !require(QDir(stagingDirectory).removeRecursively(),
                 QStringLiteral("cannot remove unpublished staging fixture"))) {
        return 1;
    }

    const auto unexpectedTransactionFile = QDir(transactionsRoot)
                                               .absoluteFilePath(QStringLiteral("unexpected.bin"));
    if (!require(writeFile(unexpectedTransactionFile, QByteArrayLiteral("unexpected")),
                 QStringLiteral("cannot create unexpected transaction-root file")) ||
        !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().size() == 1,
                 QStringLiteral("unexpected transaction-root file did not block startup")) ||
        !require(QFile::remove(unexpectedTransactionFile),
                 QStringLiteral("cannot remove unexpected transaction-root file"))) {
        return 1;
    }

#ifdef Q_OS_WIN
    const auto trailingDotAlias = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("Windows trailing-dot recovery alias rejection"),
        {{QStringLiteral("recovery./forbidden.json"),
          {false, {}},
          {true, QByteArrayLiteral("forbidden")}}});
    const auto tildeComponent = NekoGui_ConfigTransaction::Execute(
        QStringLiteral("Windows tilde-bearing component rejection"),
        {{QStringLiteral("routes_box/ROUTE~1"),
          {false, {}},
          {true, QByteArrayLiteral("forbidden")}}});
    if (!require(!trailingDotAlias.succeeded(),
                 QStringLiteral("Windows trailing-dot alias was accepted")) ||
        !require(!tildeComponent.succeeded(),
                 QStringLiteral("Windows tilde-bearing path component was accepted"))) {
        return 1;
    }
#endif

#ifdef Q_OS_WIN
    const auto hiddenPendingName = QStringLiteral("hidden-pending-test");
#else
    const auto hiddenPendingName = QStringLiteral(".hidden-pending-test");
#endif
    const auto hiddenPendingDirectory = QDir(transactionsRoot).absoluteFilePath(hiddenPendingName);
    if (!require(QDir().mkpath(hiddenPendingDirectory),
                 QStringLiteral("cannot create hidden pending directory"))) {
        return 1;
    }
    QJsonObject hiddenPendingManifest;
    hiddenPendingManifest.insert(QStringLiteral("schema"),
                                 QStringLiteral("nekoray.config_transaction.v1"));
    hiddenPendingManifest.insert(QStringLiteral("id"), hiddenPendingName);
    hiddenPendingManifest.insert(QStringLiteral("operation"),
                                 QStringLiteral("hidden pending test"));
    hiddenPendingManifest.insert(QStringLiteral("state"), QStringLiteral("prepared"));
    hiddenPendingManifest.insert(QStringLiteral("entries"), QJsonArray{});
    if (!require(writeFile(
                     QDir(hiddenPendingDirectory).absoluteFilePath(QStringLiteral("manifest.json")),
                     QJsonDocument(hiddenPendingManifest).toJson(QJsonDocument::Indented)),
                 QStringLiteral("cannot write hidden pending manifest"))) {
        return 1;
    }
#ifdef Q_OS_WIN
    const auto hiddenNative = QDir::toNativeSeparators(hiddenPendingDirectory);
    const auto hiddenAttributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(hiddenNative.utf16()));
    if (!require(hiddenAttributes != INVALID_FILE_ATTRIBUTES &&
                     SetFileAttributesW(
                         reinterpret_cast<LPCWSTR>(hiddenNative.utf16()),
                         hiddenAttributes | FILE_ATTRIBUTE_HIDDEN),
                 QStringLiteral("cannot mark pending directory hidden"))) {
        return 1;
    }
#endif
    const auto hiddenPendingIssues = NekoGui_ConfigTransaction::BlockingTransactionIssues();
#ifdef Q_OS_WIN
    SetFileAttributesW(
        reinterpret_cast<LPCWSTR>(hiddenNative.utf16()),
        hiddenAttributes & ~FILE_ATTRIBUTE_HIDDEN);
#endif
    if (!require(hiddenPendingIssues.size() == 1,
                 QStringLiteral("hidden pending transaction did not block startup")) ||
        !require(QDir(hiddenPendingDirectory).removeRecursively(),
                 QStringLiteral("cannot remove hidden pending fixture"))) {
        return 1;
    }

    const auto damagedTerminalDirectory = QDir(NekoGui_ConfigRecovery::RecoveryRootPath())
                                              .absoluteFilePath(
                                                  QStringLiteral("transactions/damaged-terminal-evidence"));
    if (!require(QDir().mkpath(damagedTerminalDirectory),
                 QStringLiteral("cannot create damaged terminal evidence fixture"))) {
        return 1;
    }
    QJsonObject damagedTerminalManifest;
    damagedTerminalManifest.insert(QStringLiteral("schema"),
                                   QStringLiteral("nekoray.config_transaction.v1"));
    damagedTerminalManifest.insert(QStringLiteral("id"),
                                   QStringLiteral("damaged-terminal-evidence"));
    damagedTerminalManifest.insert(QStringLiteral("operation"),
                                   QStringLiteral("damaged terminal evidence test"));
    damagedTerminalManifest.insert(QStringLiteral("state"), QStringLiteral("committed"));
    damagedTerminalManifest.insert(QStringLiteral("entries"), QJsonArray{});
    if (!require(writeFile(
                     QDir(damagedTerminalDirectory).absoluteFilePath(QStringLiteral("manifest.json")),
                     QJsonDocument(damagedTerminalManifest).toJson(QJsonDocument::Indented)),
                 QStringLiteral("cannot write damaged terminal evidence manifest"))) {
        return 1;
    }
    const auto damagedTerminalIssues = NekoGui_ConfigTransaction::BlockingTransactionIssues();
    const auto damagedTerminalReport = NekoGui_ConfigTransaction::BuildRecoveryReport();
    const auto damagedTerminalTransactions = QJsonDocument::fromJson(damagedTerminalReport.json)
                                                 .object()
                                                 .value(QStringLiteral("transactions"))
                                                 .toArray();
    if (!require(damagedTerminalIssues.isEmpty(),
                 QStringLiteral("damaged historical terminal evidence blocked active configuration")) ||
        !require(damagedTerminalReport.succeeded, damagedTerminalReport.error) ||
        !require(damagedTerminalTransactions.size() == 1 &&
                     !damagedTerminalTransactions.first().toObject().value(QStringLiteral("valid")).toBool(),
                 QStringLiteral("damaged terminal evidence was hidden from the recovery report")) ||
        !require(QDir(damagedTerminalDirectory).removeRecursively(),
                 QStringLiteral("cannot remove damaged terminal evidence fixture"))) {
        return 1;
    }

    const auto invalidTerminalDirectory = QDir(NekoGui_ConfigRecovery::RecoveryRootPath())
                                              .absoluteFilePath(
                                                  QStringLiteral("transactions/invalid-terminal"));
    if (!require(QDir().mkpath(invalidTerminalDirectory),
                 QStringLiteral("cannot create invalid terminal fixture"))) {
        return 1;
    }
    QJsonObject invalidTerminalManifest;
    invalidTerminalManifest.insert(QStringLiteral("schema"), QStringLiteral("invalid.schema"));
    invalidTerminalManifest.insert(QStringLiteral("id"), QStringLiteral("invalid-terminal"));
    invalidTerminalManifest.insert(QStringLiteral("operation"), QStringLiteral("invalid terminal test"));
    invalidTerminalManifest.insert(QStringLiteral("state"), QStringLiteral("committed"));
    invalidTerminalManifest.insert(QStringLiteral("entries"), QJsonArray{});
    if (!require(writeFile(
                     QDir(invalidTerminalDirectory).absoluteFilePath(QStringLiteral("manifest.json")),
                     QJsonDocument(invalidTerminalManifest).toJson(QJsonDocument::Indented)),
                 QStringLiteral("cannot write invalid terminal manifest"))) {
        return 1;
    }
    const auto invalidTerminalIssues = NekoGui_ConfigTransaction::BlockingTransactionIssues();
    const auto invalidTerminalReport = NekoGui_ConfigTransaction::BuildRecoveryReport();
    const auto invalidTerminalTransactions = QJsonDocument::fromJson(invalidTerminalReport.json)
                                                 .object()
                                                 .value(QStringLiteral("transactions"))
                                                 .toArray();
    if (!require(invalidTerminalIssues.size() == 1,
                 QStringLiteral("invalid terminal manifest did not block startup")) ||
        !require(invalidTerminalReport.succeeded, invalidTerminalReport.error) ||
        !require(invalidTerminalTransactions.size() == 1 &&
                     !invalidTerminalTransactions.first().toObject().value(QStringLiteral("valid")).toBool(),
                 QStringLiteral("invalid terminal manifest was hidden from the recovery report")) ||
        !require(QDir(invalidTerminalDirectory).removeRecursively(),
                 QStringLiteral("cannot remove invalid terminal fixture"))) {
        return 1;
    }

    const auto pendingDirectory = QDir(NekoGui_ConfigRecovery::RecoveryRootPath())
                                      .absoluteFilePath(QStringLiteral("transactions/pending-test"));
    if (!require(QDir().mkpath(pendingDirectory), QStringLiteral("cannot create pending transaction fixture"))) {
        return 1;
    }
    QJsonObject pendingManifest;
    pendingManifest.insert(QStringLiteral("schema"), QStringLiteral("nekoray.config_transaction.v1"));
    pendingManifest.insert(QStringLiteral("id"), QStringLiteral("pending-test"));
    pendingManifest.insert(QStringLiteral("operation"), QStringLiteral("interrupted test"));
    pendingManifest.insert(QStringLiteral("state"), QStringLiteral("prepared"));
    pendingManifest.insert(QStringLiteral("entries"), QJsonArray{});
    if (!require(
            writeFile(
                QDir(pendingDirectory).absoluteFilePath(QStringLiteral("manifest.json")),
                QJsonDocument(pendingManifest).toJson(QJsonDocument::Indented)),
            QStringLiteral("cannot create pending transaction manifest")) ||
        !require(NekoGui_ConfigTransaction::BlockingTransactionIssues().size() == 1,
                 QStringLiteral("pending transaction did not block startup"))) {
        return 1;
    }

    return 0;
}
