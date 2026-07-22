#include "ShareFormats.hpp"

#include <QHostAddress>

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

    ShareFormatResult IpPortUserPass(
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
        QHostAddress address;
        if (!address.setAddress(serverAddress) ||
            address.protocol() != QAbstractSocket::IPv4Protocol) {
            return {{}, ShareFormatError::AddressIsNotLiteralIpv4};
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
            QStringLiteral("%1:%2:%3:%4")
                .arg(address.toString())
                .arg(serverPort)
                .arg(username)
                .arg(password),
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
        case ShareFormatError::AddressIsNotLiteralIpv4:
            return QStringLiteral("the server is not a literal IPv4 address; DNS and IPv6 conversion are forbidden");
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
