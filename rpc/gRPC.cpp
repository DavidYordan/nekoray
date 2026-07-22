#include "gRPC.h"

#include <algorithm>
#include <utility>
#include <QStringList>

#ifndef NKR_NO_GRPC

#include "main/NekoGui.hpp"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QtEndian>
#include <QThread>
#include <QSemaphore>
#include <QAbstractNetworkCache>

namespace QtGrpc {
    const char *GrpcAcceptEncodingHeader = "grpc-accept-encoding";
    const char *AcceptEncodingHeader = "accept-encoding";
    const char *TEHeader = "te";
    const char *GrpcStatusHeader = "grpc-status";
    const char *GrpcStatusMessage = "grpc-message";
    const int GrpcMessageSizeHeaderSize = 5;

    class NoCache : public QAbstractNetworkCache {
    public:
        QNetworkCacheMetaData metaData(const QUrl &url) override {
            return {};
        }
        void updateMetaData(const QNetworkCacheMetaData &metaData) override {
        }
        QIODevice *data(const QUrl &url) override {
            return nullptr;
        }
        bool remove(const QUrl &url) override {
            return false;
        }
        [[nodiscard]] qint64 cacheSize() const override {
            return 0;
        }
        QIODevice *prepare(const QNetworkCacheMetaData &metaData) override {
            return nullptr;
        }
        void insert(QIODevice *device) override {
        }
        void clear() override {
        }
    };

    class Http2GrpcChannelPrivate {
    private:
        QThread *thread;
        QNetworkAccessManager *nm;

        QString url_base;
        QString serviceName;
        QByteArray nekoray_auth;

        // async
        QNetworkReply *post(const QString &method,
                            const QString &service,
                            const QByteArray &args,
                            const QByteArray &daemonInstanceId,
                            std::uint64_t lifecycleCommandSequence) {
            QUrl callUrl = url_base + "/" + service + "/" + method;
            // qDebug() << "Service call url: " << callUrl;

            QNetworkRequest request(callUrl);
            // request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
            // request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
            request.setAttribute(QNetworkRequest::Http2DirectAttribute, true);
#endif
            request.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String{"application/grpc"});
            request.setRawHeader("Cache-Control", "no-store");
            request.setRawHeader(GrpcAcceptEncodingHeader, QByteArray{"identity,deflate,gzip"});
            request.setRawHeader(AcceptEncodingHeader, QByteArray{"identity,gzip"});
            request.setRawHeader(TEHeader, QByteArray{"trailers"});
            request.setRawHeader("nekoray_auth", nekoray_auth);
            request.setRawHeader("nekoray_daemon_instance_id", daemonInstanceId);
            if (lifecycleCommandSequence != 0) {
                request.setRawHeader(
                    "nekoray_command_sequence",
                    QByteArray::number(lifecycleCommandSequence));
            }

            QByteArray msg(GrpcMessageSizeHeaderSize, '\0');
            *reinterpret_cast<int *>(msg.data() + 1) = qToBigEndian((int) args.size());
            msg += args;
            // qDebug() << "SEND: " << msg.size();

            QNetworkReply *networkReply = nm->post(request, msg);
            return networkReply;
        }

        static QByteArray processReply(QNetworkReply *networkReply, QNetworkReply::NetworkError &statusCode) {
            // Check if no network error occured
            if (networkReply->error() != QNetworkReply::NoError) {
                statusCode = networkReply->error();
                return {};
            }

            // Check if server answer with error
            auto errCode = networkReply->rawHeader(GrpcStatusHeader).toInt();
            if (errCode != 0) {
                QStringList errstr;
                errstr << "grpc-status error code:" << Int2String(errCode) << ", error msg:"
                       << QLatin1String(networkReply->rawHeader(GrpcStatusMessage));
                MW_show_log(errstr.join(" "));
                statusCode = QNetworkReply::NetworkError::ProtocolUnknownError;
                return {};
            }
            statusCode = QNetworkReply::NetworkError::NoError;
            return networkReply->readAll().mid(GrpcMessageSizeHeaderSize);
        }

        QNetworkReply::NetworkError call(const QString &method,
                                         const QString &service,
                                         const QByteArray &args,
                                         QByteArray &qByteArray,
                                         int timeout_ms,
                                         const QByteArray &daemonInstanceId,
                                         std::uint64_t lifecycleCommandSequence) {
            QNetworkReply *networkReply = post(
                method,
                service,
                args,
                daemonInstanceId,
                lifecycleCommandSequence);

            QTimer *abortTimer = nullptr;
            if (timeout_ms > 0) {
                abortTimer = new QTimer;
                abortTimer->setSingleShot(true);
                abortTimer->setInterval(timeout_ms);
                QObject::connect(abortTimer, &QTimer::timeout, networkReply, &QNetworkReply::abort);
                abortTimer->start();
            }

            {
                QEventLoop loop;
                QObject::connect(networkReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
                loop.exec();
            }

            if (abortTimer != nullptr) {
                abortTimer->stop();
                abortTimer->deleteLater();
            }

            auto grpcStatus = QNetworkReply::NetworkError::ProtocolUnknownError;
            qByteArray = processReply(networkReply, grpcStatus);
            // qDebug() << __func__ << "RECV: " << qByteArray.toHex() << "grpcStatus" << grpcStatus;
            // qDebug() << networkReply->rawHeaderPairs();

            networkReply->deleteLater();
            return grpcStatus;
        }

    public:
        Http2GrpcChannelPrivate(const QString &url_, const QString &nekoray_auth_, const QString &serviceName_) {
            url_base = "http://" + url_;
            nekoray_auth = nekoray_auth_.toLatin1();
            serviceName = serviceName_;
            //
            thread = new QThread;
            nm = new QNetworkAccessManager();
            nm->setCache(new NoCache);
            nm->moveToThread(thread);
            thread->start();
        }

        ~Http2GrpcChannelPrivate() {
            nm->deleteLater();
            thread->quit();
            thread->wait();
            thread->deleteLater();
        }

        QNetworkReply::NetworkError Call(const QString &methodName,
                                         const google::protobuf::Message &req, google::protobuf::Message *rsp,
                                         int timeout_ms = 0,
                                         const QString& daemonInstanceId = {},
                                         std::uint64_t lifecycleCommandSequence = 0,
                                         bool requireReady = true) {
            if (requireReady && !NekoGui::dataStore->core_running) {
                return QNetworkReply::NetworkError(-1919);
            }
            if (daemonInstanceId.isEmpty()) return QNetworkReply::NetworkError(-1920);

            std::string reqStr;
            req.SerializeToString(&reqStr);
            auto requestArray = QByteArray::fromStdString(reqStr);

            QByteArray responseArray;
            QNetworkReply::NetworkError err;
            QSemaphore completed;

            runOnUiThread(
                [&] {
                    err = call(
                        methodName,
                        serviceName,
                        requestArray,
                        responseArray,
                        timeout_ms,
                        daemonInstanceId.toLatin1(),
                        lifecycleCommandSequence);
                    completed.release();
                },
                nm);

            completed.acquire();
            // qDebug() << "rsp err" << err;
            // qDebug() << "rsp array" << responseArray;

            if (err != QNetworkReply::NetworkError::NoError) {
                return err;
            }
            if (!rsp->ParseFromArray(responseArray.data(), responseArray.size())) {
                return QNetworkReply::NetworkError(-114514);
            }
            return QNetworkReply::NetworkError::NoError;
        }
    };
} // namespace QtGrpc

namespace NekoGui_rpc {

    namespace {
        // This bounds only the GUI's wait. The server may still finish after
        // the HTTP/2 request is aborted, so callers must treat timeout as an
        // indeterminate transition. A later sequenced Stop orders after older
        // commands in the same daemon, but cross-daemon state still requires
        // the conservative reconciliation path.
        constexpr int CoreTransitionRpcTimeoutMs = 30 * 1000;
        constexpr int CoreReconcileRpcTimeoutMs = 5 * 1000;
        constexpr int CoreHandshakeRpcTimeoutMs = 2 * 1000;
        constexpr int CoreExitAckRpcTimeoutMs = 5 * 1000;
        constexpr std::uint32_t LifecycleProtocolVersion = 2;

        bool isSha256(const QByteArray& value) {
            if (value.size() != 64) return false;
            return std::all_of(value.cbegin(), value.cend(), [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            });
        }
    }

    Client::Client(
        std::function<void(const QString &)> onError,
        const QString &target,
        const QString &token,
        std::function<QString()> daemonIdentityProvider) {
        this->make_grpc_channel = [=]() { return std::make_unique<QtGrpc::Http2GrpcChannelPrivate>(target, token, "libcore.LibcoreService"); };
        this->default_grpc_channel = make_grpc_channel();
        this->onError = std::move(onError);
        this->daemon_identity_provider = std::move(daemonIdentityProvider);
    }

#define NOT_OK      \
    *rpcOK = false; \
    onError(QStringLiteral("QNetworkReply::NetworkError code: %1\n").arg(status));

    QString Client::CurrentDaemonIdentity() const {
        return daemon_identity_provider == nullptr ? QString{} : daemon_identity_provider();
    }

    DaemonExitAckResult Client::Exit(const QString& expectedDaemonInstanceId) {
        DaemonExitAckResult result;
        if (expectedDaemonInstanceId.isEmpty()) {
            result.detail = QStringLiteral("daemon Exit requires an expected instance identity");
            return result;
        }
        libcore::EmptyReq request;
        libcore::LifecycleStateResp reply;
        const auto commandSequence =
            lifecycle_command_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        result.commandSequence = commandSequence;
        const auto status = default_grpc_channel->Call(
            "Exit",
            request,
            &reply,
            CoreExitAckRpcTimeoutMs,
            expectedDaemonInstanceId,
            commandSequence);
        if (status != QNetworkReply::NoError) {
            result.detail = QStringLiteral("daemon Exit transport error %1").arg(status);
            return result;
        }
        if (QString::fromStdString(reply.daemon_instance_id()) != expectedDaemonInstanceId ||
            reply.ordering_watermark() != commandSequence ||
            reply.phase() != libcore::LIFECYCLE_PHASE_EXITING ||
            reply.has_current() || !reply.current_config_sha256().empty() ||
            reply.active_start_command_sequence() != 0 ||
            !reply.has_target_command() ||
            reply.target_command().sequence() != commandSequence ||
            reply.target_command().kind() != libcore::LIFECYCLE_COMMAND_EXIT ||
            reply.target_command().outcome() != libcore::LIFECYCLE_OUTCOME_SUCCEEDED ||
            !reply.target_command().config_sha256().empty() ||
            reply.target_command().requested_config_matches()) {
            result.detail = QStringLiteral(
                "daemon Exit acknowledgement identity, sequence, phase, or state mismatch");
            return result;
        }
        result.acknowledged = true;
        result.detail = QStringLiteral("the exact stopped daemon accepted Exit");
        return result;
    }

    bool Client::VerifyDaemon(
        bool* rpcOK,
        const QString& expectedDaemonInstanceId,
        QString* detail) {
        *rpcOK = false;
        libcore::EmptyReq request;
        libcore::DaemonInfoResp reply;
        const auto status = default_grpc_channel->Call(
            "GetDaemonInfo",
            request,
            &reply,
            CoreHandshakeRpcTimeoutMs,
            expectedDaemonInstanceId,
            0,
            false);
        if (status != QNetworkReply::NoError) {
            if (detail != nullptr) {
                *detail = QStringLiteral("daemon handshake transport error %1").arg(status);
            }
            return false;
        }
        if (QString::fromStdString(reply.daemon_instance_id()) != expectedDaemonInstanceId) {
            if (detail != nullptr) *detail = QStringLiteral("daemon handshake identity mismatch");
            return false;
        }
        if (reply.lifecycle_protocol_version() != LifecycleProtocolVersion) {
            if (detail != nullptr) {
                *detail = QStringLiteral("unsupported lifecycle protocol version %1")
                              .arg(reply.lifecycle_protocol_version());
            }
            return false;
        }
        *rpcOK = true;
        return true;
    }

    QString Client::Start(
        bool *rpcOK,
        const libcore::LoadConfigReq &request,
        const QString& expectedDaemonInstanceId,
        std::uint64_t* commandSequenceOut) {
        libcore::ErrorResp reply;
        const auto commandSequence =
            lifecycle_command_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (commandSequenceOut != nullptr) *commandSequenceOut = commandSequence;
        auto status = default_grpc_channel->Call(
            "Start",
            request,
            &reply,
            CoreTransitionRpcTimeoutMs,
            expectedDaemonInstanceId,
            commandSequence);

        if (status == QNetworkReply::NoError) {
            *rpcOK = true;
            return {reply.error().c_str()};
        } else {
            NOT_OK
            return "";
        }
    }

    QString Client::Stop(
        bool *rpcOK,
        const QString& expectedDaemonInstanceId,
        std::uint64_t* commandSequenceOut) {
        libcore::EmptyReq request;
        libcore::ErrorResp reply;
        const auto commandSequence =
            lifecycle_command_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (commandSequenceOut != nullptr) *commandSequenceOut = commandSequence;
        auto status = default_grpc_channel->Call(
            "Stop",
            request,
            &reply,
            CoreTransitionRpcTimeoutMs,
            expectedDaemonInstanceId,
            commandSequence);

        if (status == QNetworkReply::NoError) {
            *rpcOK = true;
            return {reply.error().c_str()};
        } else {
            NOT_OK
            return "";
        }
    }

    LifecycleReconcileResult Client::ReconcileStart(
        const QString& expectedDaemonInstanceId,
        std::uint64_t targetCommandSequence,
        const QByteArray& expectedConfigSha256) {
        return ReconcileLifecycle(
            expectedDaemonInstanceId,
            targetCommandSequence,
            libcore::LIFECYCLE_COMMAND_START,
            expectedConfigSha256);
    }

    LifecycleReconcileResult Client::ReconcileStop(
        const QString& expectedDaemonInstanceId,
        std::uint64_t targetCommandSequence,
        const QByteArray& expectedConfigSha256) {
        return ReconcileLifecycle(
            expectedDaemonInstanceId,
            targetCommandSequence,
            libcore::LIFECYCLE_COMMAND_STOP,
            expectedConfigSha256);
    }

    LifecycleReconcileResult Client::ReconcileExit(
        const QString& expectedDaemonInstanceId,
        std::uint64_t targetCommandSequence) {
        return ReconcileLifecycle(
            expectedDaemonInstanceId,
            targetCommandSequence,
            libcore::LIFECYCLE_COMMAND_EXIT,
            {});
    }

    LifecycleReconcileResult Client::ReconcileLifecycle(
        const QString& expectedDaemonInstanceId,
        std::uint64_t targetCommandSequence,
        libcore::LifecycleCommandKind targetCommandKind,
        const QByteArray& expectedConfigSha256) {
        LifecycleReconcileResult result;
        const auto targetIsExit =
            targetCommandKind == libcore::LIFECYCLE_COMMAND_EXIT;
        if (expectedDaemonInstanceId.isEmpty() || targetCommandSequence == 0 ||
            (targetCommandKind != libcore::LIFECYCLE_COMMAND_START &&
             targetCommandKind != libcore::LIFECYCLE_COMMAND_STOP &&
             !targetIsExit) ||
            (targetIsExit ? !expectedConfigSha256.isEmpty()
                          : !isSha256(expectedConfigSha256))) {
            result.detail = QStringLiteral("invalid lifecycle reconciliation input");
            return result;
        }

        libcore::LifecycleReconcileReq request;
        request.set_target_command_sequence(targetCommandSequence);
        request.set_target_command_kind(targetCommandKind);
        request.set_target_config_sha256(expectedConfigSha256.toStdString());
        libcore::LifecycleStateResp reply;
        const auto barrierSequence =
            lifecycle_command_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto status = default_grpc_channel->Call(
            "ReconcileLifecycle",
            request,
            &reply,
            CoreReconcileRpcTimeoutMs,
            expectedDaemonInstanceId,
            barrierSequence);
        if (status != QNetworkReply::NoError) {
            result.detail = QStringLiteral("lifecycle reconciliation transport error %1").arg(status);
            return result;
        }
        if (QString::fromStdString(reply.daemon_instance_id()) != expectedDaemonInstanceId ||
            reply.ordering_watermark() != barrierSequence ||
            !reply.has_target_command() ||
            reply.target_command().sequence() != targetCommandSequence ||
            reply.target_command().kind() != targetCommandKind) {
            result.detail = QStringLiteral("lifecycle reconciliation identity or sequence mismatch");
            return result;
        }

        const auto& target = reply.target_command();
        const auto currentHash = QByteArray::fromStdString(reply.current_config_sha256());
        const auto targetHash = QByteArray::fromStdString(target.config_sha256());
        const auto stopped = reply.phase() == libcore::LIFECYCLE_PHASE_STOPPED &&
                             !reply.has_current() && reply.current_config_sha256().empty() &&
                             reply.active_start_command_sequence() == 0;
        const auto exactCurrent = reply.phase() == libcore::LIFECYCLE_PHASE_ACTIVE &&
                                  reply.has_current() && isSha256(currentHash) &&
                                  currentHash == expectedConfigSha256;

        if (targetIsExit) {
            // This is the only state in which a lost Exit response may be
            // treated as a clean rejection. The higher-sequence barrier now
            // owns the watermark, so a delayed target Exit cannot commit
            // after this response. An acknowledged, superseded, malformed,
            // active, blocked, or EXITING result stays indeterminate.
            if (stopped &&
                target.outcome() == libcore::LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED &&
                targetHash.isEmpty() && !target.requested_config_matches()) {
                result.disposition = LifecycleReconcileDisposition::Stopped;
                result.detail = QStringLiteral(
                    "the Exit command was not admitted and is fenced while the daemon is stopped");
                return result;
            }
        } else if (targetCommandKind == libcore::LIFECYCLE_COMMAND_START) {
            if (target.outcome() == libcore::LIFECYCLE_OUTCOME_SUCCEEDED &&
                target.requested_config_matches() && targetHash == expectedConfigSha256 &&
                exactCurrent &&
                reply.active_start_command_sequence() == targetCommandSequence) {
                result.disposition = LifecycleReconcileDisposition::Active;
                result.detail = QStringLiteral("the exact Start command is active");
                return result;
            }
            if (stopped &&
                (target.outcome() == libcore::LIFECYCLE_OUTCOME_FAILED_CLEAN ||
                 target.outcome() == libcore::LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED) &&
                (target.outcome() == libcore::LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED ||
                 (target.requested_config_matches() && targetHash == expectedConfigSha256))) {
                result.disposition = LifecycleReconcileDisposition::Stopped;
                result.detail = QStringLiteral("the Start command is fenced and the daemon is stopped");
                return result;
            }
        } else {
            if (stopped &&
                (target.outcome() == libcore::LIFECYCLE_OUTCOME_SUCCEEDED ||
                 target.outcome() == libcore::LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED)) {
                result.disposition = LifecycleReconcileDisposition::Stopped;
                result.detail = QStringLiteral("the daemon is stopped after the Stop fence");
                return result;
            }
            if (exactCurrent &&
                (target.outcome() == libcore::LIFECYCLE_OUTCOME_REJECTED_CLEAN ||
                 target.outcome() == libcore::LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED)) {
                result.disposition = LifecycleReconcileDisposition::Active;
                result.detail = QStringLiteral("the prior runtime remains active after the Stop fence");
                return result;
            }
        }

        result.detail = QStringLiteral("lifecycle reconciliation returned a blocked, superseded, or inconsistent state");
        return result;
    }

    long long Client::QueryStats(const std::string &tag, const std::string &direct) {
        libcore::QueryStatsReq request;
        request.set_tag(tag);
        request.set_direct(direct);

        libcore::QueryStatsResp reply;
        auto status = default_grpc_channel->Call(
            "QueryStats", request, &reply, 500, CurrentDaemonIdentity());

        if (status == QNetworkReply::NoError) {
            return reply.traffic();
        } else {
            return 0;
        }
    }

    std::string Client::ListConnections() {
        libcore::EmptyReq request;
        libcore::ListConnectionsResp reply;
        auto status = default_grpc_channel->Call(
            "ListConnections", request, &reply, 500, CurrentDaemonIdentity());

        if (status == QNetworkReply::NoError) {
            return reply.nekoray_connections_json();
        } else {
            return "";
        }
    }

    //

    libcore::TestResp Client::Test(bool *rpcOK, const libcore::TestReq &request) {
        libcore::TestResp reply;
        auto status = make_grpc_channel()->Call(
            "Test", request, &reply, 0, CurrentDaemonIdentity());

        if (status == QNetworkReply::NoError) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return reply;
        }
    }

    libcore::UpdateResp Client::Update(bool *rpcOK, const libcore::UpdateReq &request) {
        libcore::UpdateResp reply;
        auto status = default_grpc_channel->Call(
            "Update", request, &reply, 0, CurrentDaemonIdentity());

        if (status == QNetworkReply::NoError) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return reply;
        }
    }
} // namespace NekoGui_rpc

#endif
