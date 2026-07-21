#include "main/ConfigRecovery.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

    const auto outside = NekoGui_ConfigRecovery::CreateBackupBeforeOverwrite(
        labDir.absoluteFilePath(QStringLiteral("outside.json")), sourceContent);
    if (!require(!outside.succeeded, QStringLiteral("outside-config snapshot was not rejected"))) return 1;

    if (!require(writeFile(firstBackup.snapshotPath, QByteArrayLiteral("tampered")),
                 QStringLiteral("cannot create tampered backup fixture"))) {
        return 1;
    }
    const auto collision = NekoGui_ConfigRecovery::CreateBackupBeforeOverwrite(sourcePath, sourceContent);
    if (!require(!collision.succeeded, QStringLiteral("tampered existing backup was accepted"))) return 1;

    return 0;
}
