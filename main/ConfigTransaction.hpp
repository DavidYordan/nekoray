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

    enum class RecoveryDirection {
        RestoreBefore,
        ApplyAfter,
    };

    struct RecoveryReportResult {
        bool succeeded = false;
        QByteArray json;
        QString error;
    };

    struct ManualRecoveryResult {
        bool succeeded = false;
        QString transactionPath;
        QString finalState;
        QString error;
    };

    struct MutationIntentResult {
        bool succeeded = false;
        QString transactionId;
        QString transactionPath;
        QString error;
    };

    enum class MutationIntentDisposition {
        VerifiedBefore,
        VerifiedAfter,
        Indeterminate,
    };

    struct MutationIntentCompletionResult {
        bool resolved = false;
        QString finalState;
        QString error;
    };

    // A process-reentrant, cross-process configuration-disk lock. Startup
    // holds this across the recovery scan and complete configuration load, so
    // another instance cannot publish a transaction between those steps.
    class DiskLockGuard {
    public:
        DiskLockGuard();
        ~DiskLockGuard();

        DiskLockGuard(const DiskLockGuard&) = delete;
        DiskLockGuard& operator=(const DiskLockGuard&) = delete;

        [[nodiscard]] bool acquired() const;
        [[nodiscard]] QString error() const;

    private:
        bool locked = false;
        QString failure;
    };

    [[nodiscard]] Result Execute(
        const QString& operation,
        const QList<FileMutation>& mutations);

    // Brackets a single-file QSaveFile replacement with a durable intent. The
    // caller must already hold one DiskLockGuard and keep that same guard plus
    // model-mutation serialization continuously from before Prepare succeeds
    // through exactly one Complete call on every path. The target must not be
    // mutated before Prepare succeeds. transactionId is opaque and exact-case.
    // resolved=false (including Indeterminate) leaves a durable blocker;
    // resolved=true with a non-empty error means only best-effort retirement
    // failed after the target and terminal marker were verified.
    [[nodiscard]] MutationIntentResult PrepareMutationIntent(
        const QString& operation,
        const FileMutation& mutation);

    [[nodiscard]] MutationIntentCompletionResult CompleteMutationIntent(
        const QString& transactionId,
        MutationIntentDisposition disposition,
        const QString& detail = {});

    [[nodiscard]] QStringList BlockingTransactionIssues();

    [[nodiscard]] QString RuntimeMutationBlockReason();

    // These maintenance operations run before normal configuration loading.
    // They never infer a recovery direction: callers must explicitly choose
    // the durable before or after state recorded by the transaction.
    [[nodiscard]] RecoveryReportResult BuildRecoveryReport();

    [[nodiscard]] ManualRecoveryResult Recover(
        const QString& transactionId,
        RecoveryDirection direction);

#ifdef NEKORAY_CONFIG_TRANSACTION_TESTING
    void SetApplyFailureAfterForTest(int appliedMutationCount);
#endif
} // namespace NekoGui_ConfigTransaction
