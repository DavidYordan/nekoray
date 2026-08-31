#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <functional>

namespace NekoGui_network {
    struct NekoHTTPRequestOptions {
        bool useProxy = false;
        bool proxyAvailable = false;
        int proxyPort = 0;
        QString proxyUsername;
        QString proxyPassword;
        QString userAgent;
        bool insecureTls = false;
    };

    struct NekoHTTPResponse {
        QString error;
        QByteArray data;
        QList<QPair<QByteArray, QByteArray>> header;
    };

    class NetworkRequestHelper : QObject {
        Q_OBJECT

        explicit NetworkRequestHelper(QObject* parent) : QObject(parent) {};

        ~NetworkRequestHelper() override = default;
        ;

    public:
        static NekoHTTPResponse HttpGet(
            const QUrl& url,
            const NekoHTTPRequestOptions& options);

        static QString GetHeader(const QList<QPair<QByteArray, QByteArray>>& header, const QString& name);
    };
} // namespace NekoGui_network

using namespace NekoGui_network;
