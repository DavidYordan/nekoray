#include "go/grpc_server/gen/libcore.pb.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
    constexpr auto ServiceName = "libcore.LibcoreService";
    constexpr int GrpcServerDeadlineLeadMs = 250;

    struct ProxySnapshot {
        QVariant enabled;
        QVariant server;
        QVariant autoConfigUrl;
        QVariant proxyOverride;
        QVariant autoDetect;
    };

    struct GrpcCallResult {
        bool ok = false;
        bool transportOk = false;
        bool grpcStatusPresent = false;
        bool framingOk = false;
        int networkError = 0;
        int grpcStatus = -1;
        int httpStatus = 0;
        QByteArray payload;
        QString detail;
    };

    struct FileIdentity {
        DWORD volumeSerial = 0;
        DWORD fileIndexHigh = 0;
        DWORD fileIndexLow = 0;
    };

    struct CheckedPath {
        QString lexicalPath;
        QString finalPath;
        FileIdentity identity;
    };

    class ScopedHandle {
    public:
        explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
        ~ScopedHandle() {
            if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_);
        }
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
        HANDLE get() const { return value_; }
        bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }

    private:
        HANDLE value_;
    };

    ProxySnapshot readProxySnapshot() {
        QSettings settings(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"),
            QSettings::NativeFormat);
        return {
            settings.value(QStringLiteral("ProxyEnable")),
            settings.value(QStringLiteral("ProxyServer")),
            settings.value(QStringLiteral("AutoConfigURL")),
            settings.value(QStringLiteral("ProxyOverride")),
            settings.value(QStringLiteral("AutoDetect")),
        };
    }

    bool sameProxySnapshot(const ProxySnapshot& left, const ProxySnapshot& right) {
        return left.enabled == right.enabled && left.server == right.server &&
               left.autoConfigUrl == right.autoConfigUrl &&
               left.proxyOverride == right.proxyOverride &&
               left.autoDetect == right.autoDetect;
    }

    bool sameFileIdentity(const FileIdentity& left, const FileIdentity& right) {
        return left.volumeSerial == right.volumeSerial &&
               left.fileIndexHigh == right.fileIndexHigh &&
               left.fileIndexLow == right.fileIndexLow;
    }

    bool pathInside(const QString& candidate, const QString& root) {
        const auto normalizedCandidate = QDir::toNativeSeparators(candidate).replace('/', '\\');
        auto normalizedRoot = QDir::toNativeSeparators(root).replace('/', '\\');
        while (normalizedRoot.size() > 3 && normalizedRoot.endsWith('\\')) normalizedRoot.chop(1);
        return normalizedCandidate.compare(normalizedRoot, Qt::CaseInsensitive) == 0 ||
               normalizedCandidate.startsWith(normalizedRoot + '\\', Qt::CaseInsensitive);
    }

    QString win32Error(const QString& action) {
        return QStringLiteral("%1 (Win32 error %2)").arg(action).arg(GetLastError());
    }

    bool openFinalPath(
        const QString& lexicalPath,
        bool expectDirectory,
        CheckedPath* checked,
        QString* detail) {
        const auto flags = expectDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0;
        ScopedHandle handle(CreateFileW(
            reinterpret_cast<LPCWSTR>(lexicalPath.utf16()),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            flags,
            nullptr));
        if (!handle.valid()) {
            if (detail != nullptr) *detail = win32Error(QStringLiteral("cannot open path by handle"));
            return false;
        }

        std::vector<wchar_t> finalBuffer(32768);
        const auto finalLength = GetFinalPathNameByHandleW(
            handle.get(),
            finalBuffer.data(),
            static_cast<DWORD>(finalBuffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
        if (finalLength == 0 || finalLength >= finalBuffer.size()) {
            if (detail != nullptr) *detail = win32Error(QStringLiteral("cannot resolve final path"));
            return false;
        }

        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle.get(), &information)) {
            if (detail != nullptr) *detail = win32Error(QStringLiteral("cannot resolve file identity"));
            return false;
        }
        if (checked != nullptr) {
            checked->lexicalPath = lexicalPath;
            checked->finalPath = QString::fromWCharArray(finalBuffer.data(), finalLength);
            checked->identity = {
                information.dwVolumeSerialNumber,
                information.nFileIndexHigh,
                information.nFileIndexLow,
            };
        }
        return true;
    }

    bool normalizeAndCheckExistingLocalPath(
        const QString& rawPath,
        bool expectDirectory,
        CheckedPath* checked,
        QString* detail) {
        auto nativePath = QDir::toNativeSeparators(rawPath);
        if (nativePath.size() < 3 || !nativePath.at(0).isLetter() ||
            nativePath.at(1) != ':' || nativePath.at(2) != '\\' ||
            nativePath.startsWith(QStringLiteral("\\\\")) ||
            nativePath.startsWith(QStringLiteral("\\?\\")) ||
            nativePath.startsWith(QStringLiteral("\\.\\"))) {
            if (detail != nullptr) {
                *detail = QStringLiteral("path must use a plain absolute local drive-letter namespace");
            }
            return false;
        }
        if (nativePath.mid(2).contains(':')) {
            if (detail != nullptr) *detail = QStringLiteral("path contains ADS/namespace syntax");
            return false;
        }
        const auto components = nativePath.mid(3).split('\\', Qt::KeepEmptyParts);
        if (components.isEmpty()) {
            if (detail != nullptr) *detail = QStringLiteral("drive roots are not accepted");
            return false;
        }
        for (const auto& component : components) {
            if (component.isEmpty() || component == QStringLiteral(".") ||
                component == QStringLiteral("..") || component.contains('~') ||
                component.endsWith(' ') || component.endsWith('.')) {
                if (detail != nullptr) {
                    *detail = QStringLiteral("path contains an ambiguous Windows component");
                }
                return false;
            }
        }

        std::vector<wchar_t> fullBuffer(32768);
        const auto fullLength = GetFullPathNameW(
            reinterpret_cast<LPCWSTR>(nativePath.utf16()),
            static_cast<DWORD>(fullBuffer.size()),
            fullBuffer.data(),
            nullptr);
        if (fullLength == 0 || fullLength >= fullBuffer.size()) {
            if (detail != nullptr) *detail = win32Error(QStringLiteral("cannot normalize path"));
            return false;
        }
        nativePath = QString::fromWCharArray(fullBuffer.data(), fullLength);

        const auto driveRoot = nativePath.left(3);
        if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(driveRoot.utf16())) != DRIVE_FIXED) {
            if (detail != nullptr) *detail = QStringLiteral("path is not on a fixed local drive");
            return false;
        }
        const auto driveName = nativePath.left(2);
        std::vector<wchar_t> deviceBuffer(32768);
        if (QueryDosDeviceW(
                reinterpret_cast<LPCWSTR>(driveName.utf16()),
                deviceBuffer.data(),
                static_cast<DWORD>(deviceBuffer.size())) == 0) {
            if (detail != nullptr) *detail = win32Error(QStringLiteral("cannot resolve DOS device"));
            return false;
        }
        const auto deviceTarget = QString::fromWCharArray(deviceBuffer.data());
        const auto devicePrefix = QStringLiteral("\\Device\\");
        if (!deviceTarget.startsWith(devicePrefix, Qt::CaseInsensitive) ||
            deviceTarget.mid(devicePrefix.size()).contains('\\')) {
            if (detail != nullptr) *detail = QStringLiteral("SUBST/DOS-device path aliases are forbidden");
            return false;
        }

        auto current = driveRoot;
        for (int index = 0; index < components.size(); ++index) {
            if (current.size() > 3) current += '\\';
            current += components.at(index);
            const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                if (detail != nullptr) *detail = win32Error(QStringLiteral("path component is missing"));
                return false;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                if (detail != nullptr) *detail = QStringLiteral("reparse/junction path components are forbidden");
                return false;
            }
            const auto isLast = index == components.size() - 1;
            if ((!isLast || expectDirectory) && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                if (detail != nullptr) *detail = QStringLiteral("expected directory component is not a directory");
                return false;
            }
            if (isLast && !expectDirectory && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (detail != nullptr) *detail = QStringLiteral("expected executable is a directory");
                return false;
            }
        }
        return openFinalPath(nativePath, expectDirectory, checked, detail);
    }

    QStringList protectedProductionRoots() {
        QStringList roots{QStringLiteral("D:\\Program Files\\nekoray")};
        for (const auto& variable : {"APPDATA", "LOCALAPPDATA"}) {
            const auto base = qEnvironmentVariable(variable);
            if (base.isEmpty()) continue;
            roots << QDir(base).filePath(QStringLiteral("nekoray"));
            roots << QDir(base).filePath(QStringLiteral("nekobox"));
        }
        return roots;
    }

    bool checkOutsideProtectedRoots(const CheckedPath& candidate, QString* detail) {
        for (const auto& protectedRoot : protectedProductionRoots()) {
            const auto protectedLexical = QDir::toNativeSeparators(
                QFileInfo(protectedRoot).absoluteFilePath());
            if (pathInside(candidate.lexicalPath, protectedLexical)) {
                if (detail != nullptr) *detail = QStringLiteral("path is inside a protected production root");
                return false;
            }
            if (!QFileInfo::exists(protectedLexical)) continue;
            CheckedPath checkedRoot;
            QString rootDetail;
            if (!openFinalPath(protectedLexical, true, &checkedRoot, &rootDetail)) {
                if (detail != nullptr) {
                    *detail = QStringLiteral("cannot verify protected root final identity: %1").arg(rootDetail);
                }
                return false;
            }
            if (pathInside(candidate.finalPath, checkedRoot.finalPath) ||
                sameFileIdentity(candidate.identity, checkedRoot.identity)) {
                if (detail != nullptr) *detail = QStringLiteral("path aliases a protected production root");
                return false;
            }
        }
        return true;
    }

    bool checkCoreIsNotProductionHardlink(const CheckedPath& candidate, QString* detail) {
        const auto protectedRoot = QStringLiteral("D:\\Program Files\\nekoray");
        if (!QFileInfo(protectedRoot).isDir()) return true;
        QDir directory(protectedRoot);
        const auto executables = directory.entryInfoList(
            {QStringLiteral("*.exe")},
            QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const auto& executable : executables) {
            const auto attributes = GetFileAttributesW(
                reinterpret_cast<LPCWSTR>(executable.absoluteFilePath().utf16()));
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                continue;
            }
            CheckedPath productionExecutable;
            QString ignoredDetail;
            if (!openFinalPath(executable.absoluteFilePath(), false, &productionExecutable, &ignoredDetail)) {
                if (detail != nullptr) {
                    *detail = QStringLiteral("cannot verify production executable identity");
                }
                return false;
            }
            if (sameFileIdentity(candidate.identity, productionExecutable.identity)) {
                if (detail != nullptr) *detail = QStringLiteral("core is a hardlink to a production executable");
                return false;
            }
        }
        return true;
    }

    QByteArray sha256File(const QString& path, QString* detail) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (detail != nullptr) *detail = QStringLiteral("cannot open core for SHA-256");
            return {};
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file)) {
            if (detail != nullptr) *detail = QStringLiteral("cannot hash core executable");
            return {};
        }
        return hash.result().toHex();
    }

    GrpcCallResult callGrpc(
        QNetworkAccessManager& manager,
        quint16 port,
        const QByteArray& token,
        const QByteArray& daemonInstanceId,
        const QString& method,
        const google::protobuf::Message& requestMessage,
        int timeoutMs,
        quint64 commandSequence = 0) {
        GrpcCallResult result;
        std::string serialized;
        if (!requestMessage.SerializeToString(&serialized)) {
            result.detail = QStringLiteral("request serialization failed");
            return result;
        }

        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/%2/%3")
                                        .arg(port)
                                        .arg(QString::fromLatin1(ServiceName), method)));
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
        request.setAttribute(QNetworkRequest::Http2DirectAttribute, true);
#endif
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/grpc"));
        request.setRawHeader("Cache-Control", "no-store");
        request.setRawHeader("grpc-accept-encoding", "identity");
        request.setRawHeader("accept-encoding", "identity");
        request.setRawHeader("te", "trailers");
        request.setRawHeader("nekoray_auth", token);
        request.setRawHeader("nekoray_daemon_instance_id", daemonInstanceId);
        if (commandSequence != 0) {
            request.setRawHeader("nekoray_command_sequence", QByteArray::number(commandSequence));
        }
        if (timeoutMs > 0) {
            request.setRawHeader(
                "grpc-timeout",
                QByteArray::number(std::max(1, timeoutMs - GrpcServerDeadlineLeadMs)) + 'm');
        }

        const auto serializedBytes = QByteArray::fromStdString(serialized);
        QByteArray body(5, '\0');
        qToBigEndian<quint32>(
            static_cast<quint32>(serializedBytes.size()),
            reinterpret_cast<uchar*>(body.data() + 1));
        body.append(serializedBytes);

        auto* reply = manager.post(request, body);
        QTimer timer;
        timer.setSingleShot(true);
        timer.setInterval(timeoutMs);
        QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start();
        loop.exec();
        timer.stop();

        result.networkError = static_cast<int>(reply->error());
        result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool grpcStatusOk = false;
        result.grpcStatus = reply->rawHeader("grpc-status").toInt(&grpcStatusOk);
        const auto responseBody = reply->readAll();
        const auto grpcMessage = QString::fromLatin1(reply->rawHeader("grpc-message"));
        const auto contentType = reply->rawHeader("content-type");
        reply->deleteLater();

        if (result.networkError != static_cast<int>(QNetworkReply::NoError)) {
            result.detail = QStringLiteral("network error %1").arg(result.networkError);
            return result;
        }
        if (result.httpStatus != 200 ||
            !contentType.toLower().startsWith(QByteArrayLiteral("application/grpc"))) {
            result.detail = QStringLiteral("invalid gRPC HTTP response: status %1, content-type %2")
                                .arg(result.httpStatus)
                                .arg(QString::fromLatin1(contentType));
            return result;
        }
        result.transportOk = true;
        if (!grpcStatusOk) {
            result.detail = QStringLiteral("gRPC response did not carry grpc-status");
            return result;
        }
        result.grpcStatusPresent = true;

        if (responseBody.isEmpty()) {
            // A non-OK unary response may legitimately be trailers-only.  A
            // successful unary response must still carry one data frame.
            result.framingOk = result.grpcStatus != 0;
        } else if (responseBody.size() >= 5 && responseBody.at(0) == '\0') {
            const auto payloadSize = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar*>(responseBody.constData() + 1));
            if (payloadSize == static_cast<quint32>(responseBody.size() - 5)) {
                result.payload = responseBody.mid(5);
                result.framingOk = true;
            }
        }
        if (!result.framingOk) {
            result.detail = QStringLiteral("malformed gRPC unary response frame");
            return result;
        }
        if (result.grpcStatus != 0) {
            result.detail = QStringLiteral("grpc status %1: %2")
                                .arg(result.grpcStatus)
                                .arg(grpcMessage);
            return result;
        }
        result.ok = true;
        return result;
    }

    template<typename Response>
    bool parseResponse(const GrpcCallResult& call, Response* response, QString* detail) {
        if (!call.ok) {
            if (detail != nullptr) *detail = call.detail;
            return false;
        }
        if (response == nullptr ||
            !response->ParseFromArray(call.payload.constData(), call.payload.size())) {
            if (detail != nullptr) *detail = QStringLiteral("response protobuf parsing failed");
            return false;
        }
        return true;
    }

    quint16 reserveLoopbackPort() {
        for (int attempt = 0; attempt < 8; ++attempt) {
            QTcpServer reservation;
            if (!reservation.listen(QHostAddress::LocalHost, 0)) continue;
            const auto port = reservation.serverPort();
            reservation.close();
            if (port != 2080 && port != 12080 && port <= 65535) {
                return static_cast<quint16>(port);
            }
        }
        return 0;
    }

    void logFailure(const QString& message) {
        std::fprintf(stderr, "core_exit_integration_test: %s\n", message.toUtf8().constData());
    }
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        logFailure(QStringLiteral("usage: core_exit_integration_test <nekobox_core.exe> <isolated-work-root>"));
        return 2;
    }

    if (qEnvironmentVariable("ROUTEFLUENT_CORE_EXIT_TEST_AUTHORIZATION") !=
        QStringLiteral("build_windows_package.ps1:v1")) {
        logFailure(QStringLiteral(
            "refusing direct invocation; the complete Windows packaging script is the only supported entry"));
        return 2;
    }

    const auto rawCorePath = QString::fromLocal8Bit(argv[1]);
    const auto rawWorkRoot = QString::fromLocal8Bit(argv[2]);
    const auto authorizedCorePath = qEnvironmentVariable("ROUTEFLUENT_CORE_EXIT_TEST_CORE_PATH");
    const auto authorizedWorkRoot = qEnvironmentVariable("ROUTEFLUENT_CORE_EXIT_TEST_WORK_ROOT");
    const auto authorizedCoreSha256 =
        qEnvironmentVariable("ROUTEFLUENT_CORE_EXIT_TEST_CORE_SHA256").toLatin1().toLower();
    if (authorizedCorePath.isEmpty() || authorizedWorkRoot.isEmpty() ||
        QDir::toNativeSeparators(rawCorePath).compare(
            QDir::toNativeSeparators(authorizedCorePath), Qt::CaseInsensitive) != 0 ||
        QDir::toNativeSeparators(rawWorkRoot).compare(
            QDir::toNativeSeparators(authorizedWorkRoot), Qt::CaseInsensitive) != 0) {
        logFailure(QStringLiteral("arguments do not match the package-script authorization"));
        return 2;
    }
    if (authorizedCoreSha256.size() != 64) {
        logFailure(QStringLiteral("package-script core SHA-256 authorization is malformed"));
        return 2;
    }
    for (const auto value : authorizedCoreSha256) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            logFailure(QStringLiteral("package-script core SHA-256 authorization is malformed"));
            return 2;
        }
    }

    CheckedPath checkedCore;
    CheckedPath checkedWorkRoot;
    QString pathDetail;
    if (!normalizeAndCheckExistingLocalPath(
            rawCorePath, false, &checkedCore, &pathDetail) ||
        !checkOutsideProtectedRoots(checkedCore, &pathDetail) ||
        !checkCoreIsNotProductionHardlink(checkedCore, &pathDetail)) {
        logFailure(QStringLiteral("unsafe core path: %1").arg(pathDetail));
        return 2;
    }
    if (!normalizeAndCheckExistingLocalPath(
            rawWorkRoot, true, &checkedWorkRoot, &pathDetail) ||
        !checkOutsideProtectedRoots(checkedWorkRoot, &pathDetail)) {
        logFailure(QStringLiteral("unsafe integration work root: %1").arg(pathDetail));
        return 2;
    }
    if (QFileInfo(checkedCore.lexicalPath).fileName().compare(
            QStringLiteral("nekobox_core.exe"), Qt::CaseInsensitive) != 0) {
        logFailure(QStringLiteral("authorized core must be the current package nekobox_core.exe"));
        return 2;
    }
    const auto actualCoreSha256 = sha256File(checkedCore.lexicalPath, &pathDetail);
    if (actualCoreSha256.isEmpty() || actualCoreSha256 != authorizedCoreSha256) {
        logFailure(QStringLiteral("core SHA-256 no longer matches package-script authorization: %1")
                       .arg(pathDetail));
        return 2;
    }

    QTemporaryDir workDir(
        QDir(checkedWorkRoot.lexicalPath).filePath(QStringLiteral("core-exit-XXXXXX")));
    if (!workDir.isValid()) {
        logFailure(QStringLiteral("cannot create isolated integration directory"));
        return 2;
    }
    CheckedPath checkedWorkDir;
    if (!normalizeAndCheckExistingLocalPath(
            workDir.path(), true, &checkedWorkDir, &pathDetail) ||
        !checkOutsideProtectedRoots(checkedWorkDir, &pathDetail) ||
        !pathInside(checkedWorkDir.finalPath, checkedWorkRoot.finalPath)) {
        logFailure(QStringLiteral("unsafe generated integration directory: %1").arg(pathDetail));
        return 2;
    }

    const auto corePath = checkedCore.lexicalPath;

    const auto proxyBefore = readProxySnapshot();
    const auto port = reserveLoopbackPort();
    if (port == 0) {
        logFailure(QStringLiteral("cannot reserve a non-production loopback port"));
        return 2;
    }
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
    const auto daemonId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();

    QProcess core;
    core.setWorkingDirectory(workDir.path());
    core.setProcessChannelMode(QProcess::SeparateChannels);
    auto processEnvironment = QProcessEnvironment::systemEnvironment();
    for (const auto& variable : {
             "ROUTEFLUENT_CORE_EXIT_TEST_AUTHORIZATION",
             "ROUTEFLUENT_CORE_EXIT_TEST_CORE_PATH",
             "ROUTEFLUENT_CORE_EXIT_TEST_CORE_SHA256",
             "ROUTEFLUENT_CORE_EXIT_TEST_WORK_ROOT",
         }) {
        processEnvironment.remove(QString::fromLatin1(variable));
    }
    core.setProcessEnvironment(processEnvironment);
    bool finishedSignal = false;
    int finishedExitCode = -1;
    QProcess::ExitStatus finishedExitStatus = QProcess::CrashExit;
    QObject::connect(
        &core,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        [&](int exitCode, QProcess::ExitStatus exitStatus) {
            finishedSignal = true;
            finishedExitCode = exitCode;
            finishedExitStatus = exitStatus;
        });
    core.start(
        corePath,
        {
            QStringLiteral("nekobox"),
            QStringLiteral("-port"),
            QString::number(port),
            QStringLiteral("-token"),
            QString::fromLatin1(token),
            QStringLiteral("-instance-id"),
            QString::fromLatin1(daemonId),
        });
    if (!core.waitForStarted(5000)) {
        logFailure(QStringLiteral("test core failed to start: %1").arg(core.errorString()));
        return 1;
    }
    const auto expectedPid = core.processId();

    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy::NoProxy);
    bool passed = true;
    bool authenticatedReady = false;
    QElapsedTimer readyTimer;
    readyTimer.start();
    while (readyTimer.elapsed() < 6000) {
        libcore::EmptyReq request;
        libcore::DaemonInfoResp response;
        QString detail;
        const auto call = callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("GetDaemonInfo"),
            request,
            750);
        if (parseResponse(call, &response, &detail) &&
            response.daemon_instance_id() == daemonId.toStdString() &&
            response.lifecycle_protocol_version() == 3) {
            authenticatedReady = true;
            break;
        }
        QThread::msleep(50);
    }
    if (!authenticatedReady) {
        logFailure(QStringLiteral("authenticated lifecycle-v3 handshake did not become ready"));
        passed = false;
    }

    if (passed) {
        libcore::EmptyReq request;
        const auto wrongIdentity = callGrpc(
            manager,
            port,
            token,
            QByteArrayLiteral("wrong-daemon-instance"),
            QStringLiteral("Exit"),
            request,
            1000,
            1);
        if (wrongIdentity.ok || !wrongIdentity.transportOk ||
            !wrongIdentity.grpcStatusPresent || !wrongIdentity.framingOk ||
            wrongIdentity.grpcStatus != 16 || core.state() != QProcess::Running ||
            core.processId() != expectedPid) {
            logFailure(QStringLiteral(
                "wrong daemon identity did not return exact Unauthenticated while preserving the process: %1")
                           .arg(wrongIdentity.detail));
            passed = false;
        }
    }

    if (passed) {
        libcore::LifecycleReconcileReq request;
        request.set_target_command_sequence(1);
        request.set_target_command_kind(libcore::LIFECYCLE_COMMAND_EXIT);
        request.set_target_config_sha256("");
        libcore::LifecycleStateResp response;
        QString detail;
        const auto call = callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("ReconcileLifecycle"),
            request,
            5000,
            2);
        const auto exactFence = parseResponse(call, &response, &detail) &&
                                response.daemon_instance_id() == daemonId.toStdString() &&
                                response.ordering_watermark() == 2 &&
                                response.phase() == libcore::LIFECYCLE_PHASE_STOPPED &&
                                !response.has_current() &&
                                response.current_config_sha256().empty() &&
                                response.active_start_command_sequence() == 0 &&
                                response.has_target_command() &&
                                response.target_command().sequence() == 1 &&
                                response.target_command().kind() == libcore::LIFECYCLE_COMMAND_EXIT &&
                                response.target_command().outcome() ==
                                    libcore::LIFECYCLE_OUTCOME_FENCED_NOT_ADMITTED &&
                                response.target_command().config_sha256().empty() &&
                                !response.target_command().requested_config_matches();
        if (!exactFence) {
            logFailure(QStringLiteral(
                "lost Exit reconciliation did not return the exact STOPPED/non-admission fence: %1")
                           .arg(detail));
            passed = false;
        }
    }

    if (passed) {
        libcore::EmptyReq request;
        const auto delayedExit = callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("Exit"),
            request,
            1500,
            1);
        if (delayedExit.ok || !delayedExit.transportOk ||
            !delayedExit.grpcStatusPresent || !delayedExit.framingOk ||
            delayedExit.grpcStatus != 9 || core.state() != QProcess::Running ||
            core.processId() != expectedPid) {
            logFailure(QStringLiteral(
                "the delayed Exit crossed its reconciliation barrier or changed the exact process: %1")
                           .arg(delayedExit.detail));
            passed = false;
        }
    }

    if (passed) {
        libcore::LoadConfigReq request;
        request.set_core_config(
            R"({"log":{"disabled":true},"outbounds":[{"type":"direct","tag":"integration-direct"}]})");
        libcore::ErrorResp response;
        QString detail;
        const auto call = callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("Start"),
            request,
            5000,
            3);
        if (!parseResponse(call, &response, &detail) || !response.error().empty()) {
            logFailure(QStringLiteral("minimal no-listener core Start failed: %1 / %2")
                           .arg(detail, QString::fromStdString(response.error())));
            passed = false;
        }
    }

    if (passed) {
        libcore::EmptyReq request;
        const auto activeExit = callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("Exit"),
            request,
            1500,
            4);
        if (activeExit.ok || !activeExit.transportOk ||
            !activeExit.grpcStatusPresent || !activeExit.framingOk ||
            activeExit.grpcStatus != 9 || core.state() != QProcess::Running ||
            core.processId() != expectedPid) {
            logFailure(QStringLiteral(
                "active Exit did not return exact FailedPrecondition while preserving the process: %1")
                           .arg(activeExit.detail));
            passed = false;
        }
    }

    if (passed) {
        libcore::EmptyReq request;
        libcore::ErrorResp response;
        QString detail;
        const auto call = callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("Stop"),
            request,
            5000,
            5);
        if (!parseResponse(call, &response, &detail) || !response.error().empty()) {
            logFailure(QStringLiteral("minimal core Stop failed: %1 / %2")
                           .arg(detail, QString::fromStdString(response.error())));
            passed = false;
        }
    }

    if (passed) {
        libcore::EmptyReq request;
        libcore::LifecycleStateResp response;
        QString detail;
        const auto call = callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("Exit"),
            request,
            5000,
            6);
        auto exactAck = parseResponse(call, &response, &detail) &&
                        response.daemon_instance_id() == daemonId.toStdString() &&
                         response.ordering_watermark() == 6 &&
                        response.phase() == libcore::LIFECYCLE_PHASE_EXITING &&
                        !response.has_current() &&
                        response.current_config_sha256().empty() &&
                        response.active_start_command_sequence() == 0 &&
                        response.has_target_command() &&
                         response.target_command().sequence() == 6 &&
                        response.target_command().kind() == libcore::LIFECYCLE_COMMAND_EXIT &&
                        response.target_command().outcome() == libcore::LIFECYCLE_OUTCOME_SUCCEEDED &&
                        response.target_command().config_sha256().empty() &&
                        !response.target_command().requested_config_matches();
        if (!exactAck) {
            logFailure(QStringLiteral("stopped Exit did not yield an exact EXITING ACK: %1")
                           .arg(detail));
            passed = false;
        }
    }

    if (passed && core.state() != QProcess::NotRunning && !core.waitForFinished(10000)) {
        logFailure(QStringLiteral("acknowledged test core did not finish"));
        passed = false;
    }
    if (passed && (!finishedSignal || finishedExitStatus != QProcess::NormalExit ||
                   finishedExitCode != 0 ||
                   expectedPid <= 0)) {
        logFailure(QStringLiteral("test core did not deliver an exact ACK and exact NormalExit/0"));
        passed = false;
    }

    if (core.state() != QProcess::NotRunning) {
        // This process was created by this test with a no-listener/no-TUN
        // configuration. Try the authenticated lifecycle cleanup first; use
        // QProcess termination only as a last-resort cleanup for this exact PID.
        libcore::EmptyReq request;
        libcore::ErrorResp stopResponse;
        const auto stop = callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("Stop"),
            request,
            1500,
            1000);
        (void) stopResponse.ParseFromArray(stop.payload.constData(), stop.payload.size());
        (void) callGrpc(
            manager,
            port,
            token,
            daemonId,
            QStringLiteral("Exit"),
            request,
            1500,
            1001);
        if (!core.waitForFinished(3000)) {
            if (expectedPid <= 0 || core.processId() != expectedPid) {
                logFailure(QStringLiteral(
                    "refusing fallback termination because the exact test PID no longer matches"));
                passed = false;
            } else {
                core.terminate();
                if (!core.waitForFinished(2000)) {
                    if (core.processId() != expectedPid) {
                        logFailure(QStringLiteral(
                            "refusing fallback kill because the exact test PID no longer matches"));
                        passed = false;
                    } else {
                        core.kill();
                        (void) core.waitForFinished(2000);
                    }
                }
            }
        }
    }

    const auto proxyAfter = readProxySnapshot();
    if (!sameProxySnapshot(proxyBefore, proxyAfter)) {
        logFailure(QStringLiteral(
            "common Windows WinINet proxy values changed during the isolated Exit test"));
        passed = false;
    }
    if (core.state() != QProcess::NotRunning) {
        logFailure(QStringLiteral("test-owned core process remained running"));
        passed = false;
    }

    if (!passed) {
        const auto stderrLog = QString::fromUtf8(core.readAllStandardError());
        const auto stdoutLog = QString::fromUtf8(core.readAllStandardOutput());
        if (!stderrLog.isEmpty()) logFailure(QStringLiteral("core stderr: %1").arg(stderrLog));
        if (!stdoutLog.isEmpty()) logFailure(QStringLiteral("core stdout: %1").arg(stdoutLog));
        return 1;
    }
    std::fprintf(stdout, "core_exit_integration_test: PASS\n");
    return 0;
}
