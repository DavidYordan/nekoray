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

namespace {
    bool writeFile(const QString &path, const QByteArray &content) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        return file.write(content) == content.size();
    }

    QByteArray readFile(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }

    bool require(bool condition, const QString &message) {
        if (condition) return true;
        QTextStream(stderr) << message << Qt::endl;
        return false;
    }
}

int main(int argc, char **argv) {
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
        readFile(deletion.snapshotPath + QStringLiteral(".meta.json"))).object();
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
        readFile(deletion.snapshotPath + QStringLiteral(".meta.json"))).object();
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
