#include "ShareFormats.hpp"

#include "3rdparty/base64.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringList>

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
