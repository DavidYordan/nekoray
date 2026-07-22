#pragma once

#include <QString>

namespace NekoGui_fmt {
    enum class CredentialProxyKind {
        Unsupported,
        Socks5,
        Http,
    };

    enum class ShareFormatError {
        None,
        EmptyNativeLink,
        UnsupportedProtocol,
        TlsWouldBeLost,
        AddressIsNotLiteralIpv4,
        InvalidPort,
        MissingCredentials,
        AmbiguousCredentials,
    };

    struct ShareFormatResult {
        QString text;
        ShareFormatError error = ShareFormatError::None;

        [[nodiscard]] bool ok() const {
            return error == ShareFormatError::None && !text.isEmpty();
        }
    };

    // Native links are already FullyEncoded. A literal '#' can therefore only
    // begin the URI fragment; percent-encoded data such as "%23" is preserved.
    [[nodiscard]] ShareFormatResult ShareLinkWithoutRemark(const QString& nativeLink);

    // This deliberately narrow credential-list format never performs DNS and
    // never percent-encodes delimiters. Inputs that cannot be represented
    // unambiguously are rejected rather than silently changed.
    [[nodiscard]] ShareFormatResult IpPortUserPass(
        CredentialProxyKind kind,
        const QString& serverAddress,
        int serverPort,
        const QString& username,
        const QString& password,
        bool transportUsesTls);

    [[nodiscard]] QString ShareFormatErrorDescription(ShareFormatError error);
} // namespace NekoGui_fmt
