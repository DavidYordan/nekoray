#include "ShareFormats.hpp"

#include "3rdparty/base64.h"

#include <QStringList>

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
