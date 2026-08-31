#include "ShareFormats.hpp"

#include "3rdparty/base64.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>
#include <limits>

namespace NekoGui_fmt {
    ShareFormatResult ShareLinkWithoutRemark(const QString& nativeLink) {
        if (nativeLink.isEmpty()) {
            return {{}, ShareFormatError::EmptyNativeLink};
        }
        const auto fragmentStart = nativeLink.indexOf('#');
        const auto result = fragmentStart < 0 ? nativeLink : nativeLink.left(fragmentStart);
        if (result.isEmpty()) {
            return {{}, ShareFormatError::EmptyNativeLink};
        }
        return {result, ShareFormatError::None};
    }

    SocksUserInfoResult DecodeLegacySocksBase64UserInfo(
        const QString& username,
        const QString& password) {
        if (username.isEmpty() || !password.isEmpty()) {
            return {username, password, false};
        }

        const auto decoded = Qt515Base64::QByteArray_fromBase64Encoding(
            username.toUtf8(),
            Qt515Base64::Base64Option::AbortOnBase64DecodingErrors);
        if (!decoded) {
            return {username, password, false};
        }

        const auto decodedText = QString::fromUtf8(decoded.decoded);
        if (decodedText.toUtf8() != decoded.decoded) {
            return {username, password, false};
        }
        const auto separator = decodedText.indexOf(':');
        if (separator < 0) {
            return {username, password, false};
        }

        return {
            decodedText.left(separator),
            decodedText.mid(separator + 1),
            true,
        };
    }

    V2RayNVmessParseResult ParseV2RayNVmessLink(const QString& link) {
        V2RayNVmessParseResult result;
        const auto prefix = QStringLiteral("vmess://");
        if (!link.startsWith(prefix)) {
            return result;
        }

        const auto payload = link.mid(prefix.size());
        // These delimiters belong to the URI-style format and cannot appear
        // in standard base64. Leave that format to VMessBean's URI parser.
        if (payload.contains('@') || payload.contains('?') || payload.contains('#')) {
            return result;
        }
        if (payload.isEmpty()) {
            result.error = V2RayNVmessError::InvalidBase64;
            return result;
        }
        if (payload.size() % 4 == 1) {
            result.error = V2RayNVmessError::InvalidBase64;
            return result;
        }

        const auto decoded = Qt515Base64::QByteArray_fromBase64Encoding(
            payload.toLatin1(),
            Qt515Base64::Base64Option::AbortOnBase64DecodingErrors);
        if (!decoded) {
            result.error = V2RayNVmessError::InvalidBase64;
            return result;
        }

        QJsonParseError parseError{};
        const auto document = QJsonDocument::fromJson(decoded.decoded, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            result.error = V2RayNVmessError::InvalidJson;
            return result;
        }

        const auto object = document.object();
        auto parseInteger = [&](const char* key, int defaultValue, bool* ok) {
            const auto value = object.value(QLatin1String(key));
            if (value.isUndefined() || value.isNull()) {
                *ok = true;
                return defaultValue;
            }
            if (value.isString()) {
                return value.toString().toInt(ok);
            }
            if (value.isDouble()) {
                const auto number = value.toDouble();
                if (!std::isfinite(number) || std::trunc(number) != number ||
                    number < double(std::numeric_limits<int>::min()) ||
                    number > double(std::numeric_limits<int>::max())) {
                    *ok = false;
                    return defaultValue;
                }
                const auto integer = int(number);
                *ok = true;
                return integer;
            }
            *ok = false;
            return defaultValue;
        };

        auto& fields = result.fields;
        fields.name = object.value(QStringLiteral("ps")).toString();
        fields.serverAddress = object.value(QStringLiteral("add")).toString();
        fields.uuid = object.value(QStringLiteral("id")).toString();
        fields.host = object.value(QStringLiteral("host")).toString();
        fields.path = object.value(QStringLiteral("path")).toString();
        fields.headerType = object.value(QStringLiteral("type")).toString();
        fields.sni = object.value(QStringLiteral("sni")).toString();
        fields.alpn = object.value(QStringLiteral("alpn")).toString();
        fields.fingerprint = object.value(QStringLiteral("fp")).toString();
        fields.tls = object.value(QStringLiteral("tls")).toString();
        const auto insecureValue = object.value(QStringLiteral("insecure"));
        if (insecureValue.isBool()) {
            fields.allowInsecure = insecureValue.toBool();
        } else {
            const auto insecure = insecureValue.toVariant().toString().trimmed().toLower();
            fields.allowInsecure = insecure == QStringLiteral("1") ||
                insecure == QStringLiteral("true");
        }

        bool portOk = false;
        fields.serverPort = parseInteger("port", 0, &portOk);
        if (!portOk || fields.serverPort < 1 || fields.serverPort > 65535) {
            result.error = V2RayNVmessError::InvalidPort;
            return result;
        }
        bool alterIdOk = false;
        fields.alterId = parseInteger("aid", 0, &alterIdOk);
        if (!alterIdOk) fields.alterId = 0;

        fields.network = object.value(QStringLiteral("net")).toString();
        if (fields.network.isEmpty()) fields.network = QStringLiteral("tcp");
        if (fields.network == QStringLiteral("h2")) fields.network = QStringLiteral("http");
        fields.security = object.value(QStringLiteral("scy")).toString();
        if (fields.security.isEmpty()) fields.security = QStringLiteral("auto");

        if (fields.serverAddress.trimmed().isEmpty() || fields.uuid.trimmed().isEmpty()) {
            result.error = V2RayNVmessError::MissingRequiredField;
            return result;
        }
        result.error = V2RayNVmessError::None;
        return result;
    }

    V2RayNVmessBuildResult BuildV2RayNVmessLink(const V2RayNVmessFields& fields) {
        V2RayNVmessBuildResult result;
        if (fields.serverAddress.trimmed().isEmpty() || fields.uuid.trimmed().isEmpty()) {
            return result;
        }
        if (fields.serverPort < 1 || fields.serverPort > 65535) {
            result.error = V2RayNVmessError::InvalidPort;
            return result;
        }

        auto network = fields.network;
        if (network.isEmpty()) network = QStringLiteral("tcp");
        if (network == QStringLiteral("http")) network = QStringLiteral("h2");
        auto security = fields.security;
        if (security.isEmpty()) security = QStringLiteral("auto");
        const QJsonObject object{
            {QStringLiteral("v"), QStringLiteral("2")},
            {QStringLiteral("ps"), fields.name},
            {QStringLiteral("add"), fields.serverAddress},
            {QStringLiteral("port"), QString::number(fields.serverPort)},
            {QStringLiteral("id"), fields.uuid},
            {QStringLiteral("aid"), QString::number(fields.alterId)},
            {QStringLiteral("net"), network},
            {QStringLiteral("host"), fields.host},
            {QStringLiteral("path"), fields.path},
            {QStringLiteral("type"), fields.headerType},
            {QStringLiteral("scy"), security},
            {QStringLiteral("tls"), fields.tls},
            {QStringLiteral("sni"), fields.sni},
            {QStringLiteral("alpn"), fields.alpn},
            {QStringLiteral("fp"), fields.fingerprint},
            {QStringLiteral("insecure"), fields.allowInsecure ? QStringLiteral("1") : QStringLiteral("0")},
        };
        const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
        result.link = QStringLiteral("vmess://") + QString::fromLatin1(json.toBase64());
        result.error = V2RayNVmessError::None;
        return result;
    }

    ShadowSocksShareParseResult ParseShadowSocksShareLink(const QString& link) {
        ShadowSocksShareParseResult result;
        const auto prefix = QStringLiteral("ss://");
        if (!link.startsWith(prefix)) return result;

        const auto fragmentStart = link.indexOf('#');
        const auto withoutFragment = fragmentStart < 0 ? link : link.left(fragmentStart);
        const auto body = withoutFragment.mid(prefix.size());
        if (fragmentStart >= 0) {
            result.fields.name = QUrl::fromPercentEncoding(
                link.mid(fragmentStart + 1).toUtf8());
        }

        auto decodeBase64 = [](const QString& payload, bool urlSafe, QByteArray* output) {
            if (payload.isEmpty() || payload.size() % 4 == 1) return false;
            auto options = Qt515Base64::Base64Options(
                Qt515Base64::Base64Option::AbortOnBase64DecodingErrors);
            if (urlSafe) options |= Qt515Base64::Base64Option::Base64UrlEncoding;
            const auto decoded = Qt515Base64::QByteArray_fromBase64Encoding(
                payload.toLatin1(), options);
            if (!decoded) return false;
            *output = decoded.decoded;
            return true;
        };
        auto normalizePlugin = [](QString plugin) {
            if (plugin == QStringLiteral("simple-obfs")) {
                return QStringLiteral("obfs-local");
            }
            if (plugin.startsWith(QStringLiteral("simple-obfs;"))) {
                plugin.replace(0, QStringLiteral("simple-obfs").size(), QStringLiteral("obfs-local"));
            }
            return plugin;
        };
        auto validRequiredFields = [&] {
            return !result.fields.serverAddress.trimmed().isEmpty() &&
                !result.fields.method.isEmpty() && !result.fields.password.isEmpty();
        };

        if (body.contains('@')) {
            // Pre-validate the authority port so QUrl's generic invalid status
            // does not hide a precise protocol error.
            const auto authorityEndCandidates = QStringList{
                QStringLiteral("/"), QStringLiteral("?"),
            };
            auto authorityEnd = body.size();
            for (const auto& delimiter : authorityEndCandidates) {
                const auto index = body.indexOf(delimiter);
                if (index >= 0 && index < authorityEnd) authorityEnd = index;
            }
            const auto authority = body.left(authorityEnd);
            const auto serverPart = authority.mid(authority.lastIndexOf('@') + 1);
            const auto portSeparator = serverPart.lastIndexOf(':');
            bool portOk = false;
            const auto explicitPort = portSeparator >= 0
                ? serverPart.mid(portSeparator + 1).toInt(&portOk)
                : 0;
            if (!portOk || explicitPort < 1 || explicitPort > 65535) {
                result.error = ShadowSocksShareError::InvalidPort;
                return result;
            }

            const QUrl url(link, QUrl::StrictMode);
            if (!url.isValid()) {
                result.error = ShadowSocksShareError::InvalidSyntax;
                return result;
            }
            result.fields.serverAddress = url.host(QUrl::FullyDecoded);
            result.fields.serverPort = explicitPort;

            const auto encodedUserInfo = authority.left(authority.lastIndexOf('@'));
            if (encodedUserInfo.contains(':')) {
                result.fields.method = url.userName(QUrl::FullyDecoded);
                result.fields.password = url.password(QUrl::FullyDecoded);
            } else {
                QByteArray decodedUserInfo;
                if (!decodeBase64(url.userName(QUrl::FullyDecoded), true, &decodedUserInfo)) {
                    result.error = ShadowSocksShareError::InvalidBase64;
                    return result;
                }
                const auto decodedText = QString::fromUtf8(decodedUserInfo);
                if (decodedText.toUtf8() != decodedUserInfo) {
                    result.error = ShadowSocksShareError::InvalidSyntax;
                    return result;
                }
                const auto decodedSeparator = decodedText.indexOf(':');
                if (decodedSeparator < 0) {
                    result.error = ShadowSocksShareError::InvalidSyntax;
                    return result;
                }
                result.fields.method = decodedText.left(decodedSeparator);
                result.fields.password = decodedText.mid(decodedSeparator + 1);
            }

            const QUrlQuery query(url);
            result.fields.plugin = normalizePlugin(
                query.queryItemValue(QStringLiteral("plugin"), QUrl::FullyDecoded));
        } else {
            QByteArray decodedPayload;
            if (!decodeBase64(body, false, &decodedPayload) &&
                !decodeBase64(body, true, &decodedPayload)) {
                result.error = ShadowSocksShareError::InvalidBase64;
                return result;
            }
            const auto decodedText = QString::fromUtf8(decodedPayload);
            if (decodedText.toUtf8() != decodedPayload) {
                result.error = ShadowSocksShareError::InvalidSyntax;
                return result;
            }

            const auto methodSeparator = decodedText.indexOf(':');
            const auto serverSeparator = decodedText.lastIndexOf('@');
            if (methodSeparator <= 0 || serverSeparator <= methodSeparator + 1) {
                result.error = ShadowSocksShareError::InvalidSyntax;
                return result;
            }
            const auto serverPort = decodedText.mid(serverSeparator + 1);
            QString portText;
            if (serverPort.startsWith('[')) {
                const auto bracketEnd = serverPort.indexOf(']');
                if (bracketEnd <= 1 || bracketEnd + 1 >= serverPort.size() ||
                    serverPort.at(bracketEnd + 1) != ':') {
                    result.error = ShadowSocksShareError::InvalidSyntax;
                    return result;
                }
                result.fields.serverAddress = serverPort.mid(1, bracketEnd - 1);
                portText = serverPort.mid(bracketEnd + 2);
            } else {
                const auto portSeparator = serverPort.lastIndexOf(':');
                if (portSeparator <= 0) {
                    result.error = ShadowSocksShareError::InvalidSyntax;
                    return result;
                }
                result.fields.serverAddress = serverPort.left(portSeparator);
                portText = serverPort.mid(portSeparator + 1);
            }
            bool portOk = false;
            result.fields.serverPort = portText.toInt(&portOk);
            if (!portOk || result.fields.serverPort < 1 || result.fields.serverPort > 65535) {
                result.error = ShadowSocksShareError::InvalidPort;
                return result;
            }
            result.fields.method = decodedText.left(methodSeparator);
            result.fields.password = decodedText.mid(
                methodSeparator + 1,
                serverSeparator - methodSeparator - 1);
        }

        if (!validRequiredFields()) {
            result.error = ShadowSocksShareError::MissingRequiredField;
            return result;
        }
        result.error = ShadowSocksShareError::None;
        return result;
    }

    ShadowSocksShareBuildResult BuildShadowSocksShareLink(
        const ShadowSocksShareFields& fields) {
        ShadowSocksShareBuildResult result;
        if (fields.serverAddress.trimmed().isEmpty() ||
            fields.method.isEmpty() || fields.password.isEmpty()) {
            return result;
        }
        if (fields.serverPort < 1 || fields.serverPort > 65535) {
            result.error = ShadowSocksShareError::InvalidPort;
            return result;
        }

        QString encodedUserInfo;
        if (fields.method.startsWith(QStringLiteral("2022-"))) {
            encodedUserInfo = QString::fromLatin1(QUrl::toPercentEncoding(fields.method)) + ':' +
                QString::fromLatin1(QUrl::toPercentEncoding(fields.password));
        } else {
            const auto userInfo = (fields.method + ':' + fields.password)
                                      .toUtf8()
                                      .toBase64(QByteArray::Base64UrlEncoding |
                                                QByteArray::OmitTrailingEquals);
            encodedUserInfo = QString::fromLatin1(userInfo);
        }

        auto plugin = fields.plugin;
        if (plugin == QStringLiteral("simple-obfs")) {
            plugin = QStringLiteral("obfs-local");
        } else if (plugin.startsWith(QStringLiteral("simple-obfs;"))) {
            plugin.replace(0, QStringLiteral("simple-obfs").size(), QStringLiteral("obfs-local"));
        }
        QUrl endpoint;
        endpoint.setScheme(QStringLiteral("ss"));
        endpoint.setHost(fields.serverAddress);
        endpoint.setPort(fields.serverPort);
        if (!plugin.isEmpty()) endpoint.setPath(QStringLiteral("/"));

        if (!endpoint.isValid()) {
            result.error = ShadowSocksShareError::InvalidSyntax;
            return result;
        }
        const auto endpointText = endpoint.toString(QUrl::FullyEncoded);
        result.link = QStringLiteral("ss://") + encodedUserInfo + '@' +
            endpointText.mid(QStringLiteral("ss://").size());
        if (!plugin.isEmpty()) {
            result.link += QStringLiteral("?plugin=") +
                QString::fromLatin1(QUrl::toPercentEncoding(plugin));
        }
        if (!fields.name.isEmpty()) {
            result.link += '#' + QString::fromLatin1(QUrl::toPercentEncoding(fields.name));
        }
        result.error = ShadowSocksShareError::None;
        return result;
    }

    QString V2RayPluginFromClash(
        const QString& mode,
        const QString& host,
        const QString& path,
        bool tls) {
        const auto escapeValue = [](const QString& value) {
            QString escaped;
            escaped.reserve(value.size() * 2);
            for (const auto character : value) {
                if (character == '\\' || character == ';' || character == '=' ||
                    character == ',' || character == ':') {
                    escaped += '\\';
                }
                escaped += character;
            }
            return escaped;
        };

        QStringList options{QStringLiteral("v2ray-plugin")};
        if (!mode.isEmpty() && mode != QStringLiteral("websocket")) {
            options += QStringLiteral("mode=") + escapeValue(mode);
        }
        if (tls) options += QStringLiteral("tls");
        if (!host.isEmpty()) options += QStringLiteral("host=") + escapeValue(host);
        if (!path.isEmpty()) options += QStringLiteral("path=") + escapeValue(path);
        return options.join(';');
    }

    ShareFormatResult ServerPortUserPass(
        CredentialProxyKind kind,
        const QString& serverAddress,
        int serverPort,
        const QString& username,
        const QString& password,
        bool transportUsesTls) {
        if (kind != CredentialProxyKind::Socks5 && kind != CredentialProxyKind::Http) {
            return {{}, ShareFormatError::UnsupportedProtocol};
        }
        if (transportUsesTls) {
            return {{}, ShareFormatError::TlsWouldBeLost};
        }
        auto ambiguousServer = serverAddress.isEmpty() || serverAddress.contains(':');
        for (const auto character: serverAddress) {
            if (character.isSpace()) {
                ambiguousServer = true;
                break;
            }
        }
        if (ambiguousServer) {
            return {{}, ShareFormatError::AmbiguousServerAddress};
        }
        if (serverPort < 1 || serverPort > 65535) {
            return {{}, ShareFormatError::InvalidPort};
        }
        if (username.isEmpty() || password.isEmpty()) {
            return {{}, ShareFormatError::MissingCredentials};
        }
        const auto ambiguous = [](const QString& value) {
            return value.contains(':') || value.contains('\r') || value.contains('\n');
        };
        if (ambiguous(username) || ambiguous(password)) {
            return {{}, ShareFormatError::AmbiguousCredentials};
        }
        return {
            QStringList{
                serverAddress,
                QString::number(serverPort),
                username,
                password,
            }.join(':'),
            ShareFormatError::None,
        };
    }

    QString ShareFormatErrorDescription(ShareFormatError error) {
        switch (error) {
        case ShareFormatError::None:
            return {};
        case ShareFormatError::EmptyNativeLink:
            return QStringLiteral("the profile has no native share link");
        case ShareFormatError::UnsupportedProtocol:
            return QStringLiteral("only SOCKS5 and HTTP profiles support this format");
        case ShareFormatError::TlsWouldBeLost:
            return QStringLiteral("the profile uses TLS, which this format cannot represent");
        case ShareFormatError::AmbiguousServerAddress:
            return QStringLiteral("the server field is empty or contains whitespace, a colon, or a line break; IPv6 cannot be represented unambiguously by this format");
        case ShareFormatError::InvalidPort:
            return QStringLiteral("the server port is outside 1-65535");
        case ShareFormatError::MissingCredentials:
            return QStringLiteral("both username and password are required");
        case ShareFormatError::AmbiguousCredentials:
            return QStringLiteral("username and password must not contain colon or line breaks");
        }
        return QStringLiteral("unknown share-format error");
    }
} // namespace NekoGui_fmt
