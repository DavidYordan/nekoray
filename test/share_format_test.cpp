#include "fmt/ShareFormats.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QUrl>

namespace {
    bool expect(bool condition, const char* message) {
        if (!condition) qCritical() << message;
        return condition;
    }
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using namespace NekoGui_fmt;
    bool ok = true;

    const auto withoutRemark = ShareLinkWithoutRemark(
        QStringLiteral("socks5://user:p%23ass@192.0.2.11:1081#test-01"));
    ok &= expect(withoutRemark.ok() &&
                     withoutRemark.text ==
                         QStringLiteral("socks5://user:p%23ass@192.0.2.11:1081"),
                 "fragment removal must preserve the encoded native link");
    const auto alreadyBare = ShareLinkWithoutRemark(
        QStringLiteral("anytls://secret@192.0.2.2:443?insecure=1"));
    ok &= expect(alreadyBare.ok() &&
                     alreadyBare.text ==
                         QStringLiteral("anytls://secret@192.0.2.2:443?insecure=1"),
                 "a link without a fragment must remain byte-for-byte unchanged");
    ok &= expect(ShareLinkWithoutRemark({}).error == ShareFormatError::EmptyNativeLink,
                 "an absent native link must fail explicitly");

    const auto legacySocksUserInfo = DecodeLegacySocksBase64UserInfo(
        QStringLiteral("bGVnYWN5LXVzZXI6bGVnYWN5LXBhc3M="),
        {});
    ok &= expect(legacySocksUserInfo.decodedLegacyBase64 &&
                     legacySocksUserInfo.username == QStringLiteral("legacy-user") &&
                     legacySocksUserInfo.password == QStringLiteral("legacy-pass"),
                 "legacy SOCKS base64 userinfo must decode into username and password");

    const auto colonPassword = DecodeLegacySocksBase64UserInfo(
        QStringLiteral("dXNlcjpwOmE="),
        {});
    ok &= expect(colonPassword.decodedLegacyBase64 &&
                     colonPassword.username == QStringLiteral("user") &&
                     colonPassword.password == QStringLiteral("p:a"),
                 "only the first decoded colon separates SOCKS username and password");

    const auto invalidBase64 = DecodeLegacySocksBase64UserInfo(QStringLiteral("%%%"), {});
    const auto noSeparator = DecodeLegacySocksBase64UserInfo(QStringLiteral("bm9zZXBhcmF0b3I="), {});
    const auto explicitPassword = DecodeLegacySocksBase64UserInfo(
        QStringLiteral("bGVnYWN5LXVzZXI6bGVnYWN5LXBhc3M="),
        QStringLiteral("explicit-pass"));
    for (const auto& preserved : {invalidBase64, noSeparator, explicitPassword}) {
        ok &= expect(!preserved.decodedLegacyBase64,
                     "invalid, separator-free, or explicit SOCKS userinfo must not be reinterpreted");
    }
    ok &= expect(invalidBase64.username == QStringLiteral("%%%") && invalidBase64.password.isEmpty(),
                 "invalid base64 SOCKS userinfo must remain unchanged");
    ok &= expect(noSeparator.username == QStringLiteral("bm9zZXBhcmF0b3I=") && noSeparator.password.isEmpty(),
                 "separator-free decoded SOCKS userinfo must remain unchanged");
    ok &= expect(explicitPassword.username == QStringLiteral("bGVnYWN5LXVzZXI6bGVnYWN5LXBhc3M=") &&
                     explicitPassword.password == QStringLiteral("explicit-pass"),
                 "an explicit SOCKS password must take precedence over legacy detection");

    const auto invalidUtf8 = DecodeLegacySocksBase64UserInfo(
        QString::fromLatin1(QByteArray("\xff:\xfe", 3).toBase64()),
        {});
    ok &= expect(!invalidUtf8.decodedLegacyBase64,
                 "legacy SOCKS userinfo with invalid UTF-8 must remain unchanged");

    const QUrl legacySocksLink(
        QStringLiteral("socks://bGVnYWN5LXVzZXI6bGVnYWN5LXBhc3M=@proxy.example:1080#legacy"));
    const auto parsedLegacy = DecodeLegacySocksBase64UserInfo(
        legacySocksLink.userName(),
        legacySocksLink.password());
    QUrl canonicalSocksLink;
    canonicalSocksLink.setScheme(QStringLiteral("socks5"));
    canonicalSocksLink.setUserName(parsedLegacy.username);
    canonicalSocksLink.setPassword(parsedLegacy.password);
    canonicalSocksLink.setHost(legacySocksLink.host());
    canonicalSocksLink.setPort(legacySocksLink.port());
    canonicalSocksLink.setFragment(legacySocksLink.fragment());
    const auto canonicalSocksText = canonicalSocksLink.toString(QUrl::FullyEncoded);
    const QUrl reparsedCanonical(canonicalSocksText);
    ok &= expect(parsedLegacy.decodedLegacyBase64 &&
                     canonicalSocksText == QStringLiteral(
                         "socks5://legacy-user:legacy-pass@proxy.example:1080#legacy") &&
                     reparsedCanonical.userName() == QStringLiteral("legacy-user") &&
                     reparsedCanonical.password() == QStringLiteral("legacy-pass") &&
                     reparsedCanonical.host() == QStringLiteral("proxy.example") &&
                     reparsedCanonical.port() == 1080 &&
                     reparsedCanonical.fragment() == QStringLiteral("legacy"),
                 "legacy SOCKS userinfo must survive parse, canonical export, and reparse");

    const auto socks = ServerPortUserPass(
        CredentialProxyKind::Socks5,
        QStringLiteral("192.0.2.10"),
        1080,
        QStringLiteral("demo-user"),
        QStringLiteral("demo-pass"),
        false);
    ok &= expect(socks.ok() &&
                     socks.text == QStringLiteral(
                         "192.0.2.10:1080:demo-user:demo-pass"),
                 "SOCKS5 credentials must use the requested field order");
    ok &= expect(ServerPortUserPass(
                     CredentialProxyKind::Http,
                     QStringLiteral("192.0.2.10"),
                     8080,
                     QStringLiteral("user"),
                     QStringLiteral("pass"),
                     false)
                     .ok(),
                 "plain HTTP credentials must be representable");
    ok &= expect(ServerPortUserPass(
                     CredentialProxyKind::Socks5,
                     QStringLiteral("proxy.example"),
                     1080,
                     QStringLiteral("user%3"),
                     QStringLiteral("pass%1"),
                     false)
                         .text == QStringLiteral("proxy.example:1080:user%3:pass%1"),
                 "percent sequences in credentials must be preserved verbatim");
    ok &= expect(ServerPortUserPass(
                     CredentialProxyKind::Http,
                     QStringLiteral("192.0.2.10"),
                     443,
                     QStringLiteral("user"),
                     QStringLiteral("pass"),
                     true)
                         .error == ShareFormatError::TlsWouldBeLost,
                 "TLS HTTP must be rejected because the flat format loses TLS");
    ok &= expect(ServerPortUserPass(
                     CredentialProxyKind::Unsupported,
                     QStringLiteral("192.0.2.10"),
                     443,
                     QStringLiteral("user"),
                     QStringLiteral("pass"),
                     false)
                         .error == ShareFormatError::UnsupportedProtocol,
                 "unrelated protocols must not be flattened as proxy credentials");
    for (const auto& address : {QStringLiteral("proxy.example"), QStringLiteral("proxy-host")}) {
        const auto domain = ServerPortUserPass(
            CredentialProxyKind::Socks5,
            address,
            1080,
            QStringLiteral("user"),
            QStringLiteral("pass"),
            false);
        ok &= expect(domain.ok() &&
                         domain.text == QStringLiteral("%1:1080:user:pass").arg(address),
                     "an unresolved server name must be preserved without DNS conversion");
    }
    for (const auto& address : {QString{}, QStringLiteral(" proxy.example"),
                                QStringLiteral("proxy host"), QStringLiteral("2001:db8::1"),
                                QStringLiteral("proxy.example\nnext")}) {
        ok &= expect(ServerPortUserPass(
                         CredentialProxyKind::Socks5,
                         address,
                         1080,
                         QStringLiteral("user"),
                         QStringLiteral("pass"),
                         false)
                             .error == ShareFormatError::AmbiguousServerAddress,
                     "ambiguous server fields must be rejected without DNS conversion");
    }
    for (const auto port : {0, 65536}) {
        ok &= expect(ServerPortUserPass(
                         CredentialProxyKind::Socks5,
                         QStringLiteral("192.0.2.10"),
                         port,
                         QStringLiteral("user"),
                         QStringLiteral("pass"),
                         false)
                             .error == ShareFormatError::InvalidPort,
                     "invalid ports must be rejected");
    }
    ok &= expect(ServerPortUserPass(
                     CredentialProxyKind::Socks5,
                     QStringLiteral("192.0.2.10"),
                     1080,
                     {},
                     QStringLiteral("pass"),
                     false)
                         .error == ShareFormatError::MissingCredentials,
                 "missing credentials must be rejected");
    for (const auto& value : {QStringLiteral("user:name"), QStringLiteral("line\nbreak")}) {
        ok &= expect(ServerPortUserPass(
                         CredentialProxyKind::Socks5,
                         QStringLiteral("192.0.2.10"),
                         1080,
                         value,
                         QStringLiteral("pass"),
                         false)
                             .error == ShareFormatError::AmbiguousCredentials,
                     "ambiguous credentials must be rejected");
    }

    return ok ? 0 : 1;
}
