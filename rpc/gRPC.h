#pragma once

#ifndef NKR_NO_GRPC

#include "go/grpc_server/gen/libcore.pb.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <QByteArray>
#include <QString>

namespace QtGrpc {
    class Http2GrpcChannelPrivate;
}

namespace NekoGui_rpc {
    enum class LifecycleReconcileDisposition {
        Indeterminate,
        Active,
        Stopped,
    };

    struct LifecycleReconcileResult {
        LifecycleReconcileDisposition disposition =
            LifecycleReconcileDisposition::Indeterminate;
        QString detail;
    };

    class Client {
    public:
        explicit Client(
            std::function<void(const QString &)> onError,
            const QString &target,
            const QString &token,
            std::function<QString()> daemonIdentityProvider);

        void Exit(const QString& expectedDaemonInstanceId);

        bool VerifyDaemon(
            bool* rpcOK,
            const QString& expectedDaemonInstanceId,
            QString* detail = nullptr);

        bool KeepAlive();

        // QString returns is error string

        QString Start(
            bool *rpcOK,
            const libcore::LoadConfigReq &request,
            const QString& expectedDaemonInstanceId,
            std::uint64_t* commandSequence);

        QString Stop(
            bool *rpcOK,
            const QString& expectedDaemonInstanceId,
            std::uint64_t* commandSequence);

        LifecycleReconcileResult ReconcileStart(
            const QString& expectedDaemonInstanceId,
            std::uint64_t targetCommandSequence,
            const QByteArray& expectedConfigSha256);

        LifecycleReconcileResult ReconcileStop(
            const QString& expectedDaemonInstanceId,
            std::uint64_t targetCommandSequence,
            const QByteArray& expectedConfigSha256);

        long long QueryStats(const std::string &tag, const std::string &direct);

        std::string ListConnections();

        libcore::TestResp Test(bool *rpcOK, const libcore::TestReq &request);

        libcore::UpdateResp Update(bool *rpcOK, const libcore::UpdateReq &request);

    private:
        std::function<std::unique_ptr<QtGrpc::Http2GrpcChannelPrivate>()> make_grpc_channel;
        std::unique_ptr<QtGrpc::Http2GrpcChannelPrivate> default_grpc_channel;
        std::function<void(const QString &)> onError;
        std::function<QString()> daemon_identity_provider;
        std::atomic_uint64_t lifecycle_command_sequence{0};

        [[nodiscard]] QString CurrentDaemonIdentity() const;

        LifecycleReconcileResult ReconcileLifecycle(
            const QString& expectedDaemonInstanceId,
            std::uint64_t targetCommandSequence,
            libcore::LifecycleCommandKind targetCommandKind,
            const QByteArray& expectedConfigSha256);
    };

    inline Client *defaultClient;
} // namespace NekoGui_rpc
#endif
