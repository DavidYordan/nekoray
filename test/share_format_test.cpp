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

    const auto v2rayNJson = QByteArrayLiteral(
        R"({"v":"2","ps":"VMess 示例","add":"vmess.example","port":"443","id":"11111111-2222-3333-4444-555555555555","aid":"7","net":"h2","host":"cdn.example","path":"/edge","type":"http","scy":"auto","tls":"tls","sni":"sni.example","alpn":"h2,http/1.1","fp":"chrome","insecure":"1"})");
    const auto v2rayNLink = QStringLiteral("vmess://") +
        QString::fromLatin1(v2rayNJson.toBase64());
    const auto parsedV2RayN = ParseV2RayNVmessLink(v2rayNLink);
    ok &= expect(parsedV2RayN.ok() &&
                     parsedV2RayN.fields.name == QStringLiteral("VMess 示例") &&
                     parsedV2RayN.fields.serverAddress == QStringLiteral("vmess.example") &&
                     parsedV2RayN.fields.serverPort == 443 &&
                     parsedV2RayN.fields.uuid == QStringLiteral("11111111-2222-3333-4444-555555555555") &&
                     parsedV2RayN.fields.alterId == 7 &&
                     parsedV2RayN.fields.network == QStringLiteral("http") &&
                     parsedV2RayN.fields.host == QStringLiteral("cdn.example") &&
                     parsedV2RayN.fields.path == QStringLiteral("/edge") &&
                     parsedV2RayN.fields.headerType == QStringLiteral("http") &&
                     parsedV2RayN.fields.security == QStringLiteral("auto") &&
                     parsedV2RayN.fields.tls == QStringLiteral("tls") &&
                     parsedV2RayN.fields.sni == QStringLiteral("sni.example") &&
                     parsedV2RayN.fields.alpn == QStringLiteral("h2,http/1.1") &&
                     parsedV2RayN.fields.fingerprint == QStringLiteral("chrome") &&
                     parsedV2RayN.fields.allowInsecure,
                 "v2rayN VMess base64 JSON must preserve supported fields and normalize h2");

    const auto rebuiltV2RayN = BuildV2RayNVmessLink(parsedV2RayN.fields);
    const auto reparsedV2RayN = ParseV2RayNVmessLink(rebuiltV2RayN.link);
    ok &= expect(rebuiltV2RayN.ok() && reparsedV2RayN.ok() &&
                     reparsedV2RayN.fields.name == parsedV2RayN.fields.name &&
                     reparsedV2RayN.fields.serverAddress == parsedV2RayN.fields.serverAddress &&
                     reparsedV2RayN.fields.serverPort == parsedV2RayN.fields.serverPort &&
                     reparsedV2RayN.fields.uuid == parsedV2RayN.fields.uuid &&
                     reparsedV2RayN.fields.alterId == parsedV2RayN.fields.alterId &&
                     reparsedV2RayN.fields.network == parsedV2RayN.fields.network &&
                     reparsedV2RayN.fields.host == parsedV2RayN.fields.host &&
                     reparsedV2RayN.fields.path == parsedV2RayN.fields.path &&
                     reparsedV2RayN.fields.headerType == parsedV2RayN.fields.headerType &&
                     reparsedV2RayN.fields.security == parsedV2RayN.fields.security &&
                     reparsedV2RayN.fields.tls == parsedV2RayN.fields.tls &&
                     reparsedV2RayN.fields.sni == parsedV2RayN.fields.sni &&
                     reparsedV2RayN.fields.alpn == parsedV2RayN.fields.alpn &&
                     reparsedV2RayN.fields.fingerprint == parsedV2RayN.fields.fingerprint &&
                     reparsedV2RayN.fields.allowInsecure == parsedV2RayN.fields.allowInsecure,
                 "v2rayN VMess fields must survive parse, export, and reparse");

    const auto invalidV2RayNBase64 = ParseV2RayNVmessLink(QStringLiteral("vmess://%%%"));
    const auto invalidV2RayNLength = ParseV2RayNVmessLink(QStringLiteral("vmess://A"));
    const auto invalidV2RayNJson = ParseV2RayNVmessLink(
        QStringLiteral("vmess://") + QString::fromLatin1(QByteArrayLiteral("not-json").toBase64()));
    const auto missingV2RayNId = ParseV2RayNVmessLink(
        QStringLiteral("vmess://") + QString::fromLatin1(
            QByteArrayLiteral(R"({"add":"vmess.example","port":"443"})").toBase64()));
    const auto invalidV2RayNPort = ParseV2RayNVmessLink(
        QStringLiteral("vmess://") + QString::fromLatin1(
            QByteArrayLiteral(R"({"add":"vmess.example","port":"70000","id":"id"})").toBase64()));
    const auto fractionalV2RayNPort = ParseV2RayNVmessLink(
        QStringLiteral("vmess://") + QString::fromLatin1(
            QByteArrayLiteral(R"({"add":"vmess.example","port":443.5,"id":"id"})").toBase64()));
    const auto numericV2RayNFields = ParseV2RayNVmessLink(
        QStringLiteral("vmess://") + QString::fromLatin1(
            QByteArrayLiteral(R"({"add":"vmess.example","port":8443,"id":"id","aid":0})").toBase64()));
    ok &= expect(invalidV2RayNBase64.error == V2RayNVmessError::InvalidBase64 &&
                     invalidV2RayNLength.error == V2RayNVmessError::InvalidBase64 &&
                     invalidV2RayNJson.error == V2RayNVmessError::InvalidJson &&
                     missingV2RayNId.error == V2RayNVmessError::MissingRequiredField &&
                     invalidV2RayNPort.error == V2RayNVmessError::InvalidPort &&
                     fractionalV2RayNPort.error == V2RayNVmessError::InvalidPort &&
                     numericV2RayNFields.ok() &&
                     numericV2RayNFields.fields.serverPort == 8443 &&
                     numericV2RayNFields.fields.alterId == 0 &&
                     ParseV2RayNVmessLink(QStringLiteral(
                         "vmess://uuid@vmess.example:443?type=ws&security=tls"))
                             .error == V2RayNVmessError::NotV2RayN,
                 "malformed v2rayN JSON must fail without consuming modern VMess URIs");

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
