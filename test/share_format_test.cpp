#include "fmt/ShareFormats.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QUrl>

#include <cstdio>

namespace {
    bool expect(bool condition, const char* message) {
        if (!condition) {
            qCritical() << message;
            std::fprintf(stderr, "%s\n", message);
        }
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

    ShadowSocksShareFields shadowsocksFields;
    shadowsocksFields.name = QStringLiteral("SS 示例");
    shadowsocksFields.serverAddress = QStringLiteral("ss.example");
    shadowsocksFields.serverPort = 8388;
    shadowsocksFields.method = QStringLiteral("aes-256-gcm");
    shadowsocksFields.password = QStringLiteral("pass:word");
    shadowsocksFields.plugin = QStringLiteral(
        R"(v2ray-plugin;mode=websocket;host=cdn.example;path=/edge\=a\,b;tls;mux=0)");
    const auto builtShadowsocks = BuildShadowSocksShareLink(shadowsocksFields);
    const auto parsedShadowsocks = ParseShadowSocksShareLink(builtShadowsocks.link);
    ok &= expect(builtShadowsocks.ok() &&
                     !builtShadowsocks.link.contains(QLatin1String("%3D@")) &&
                     parsedShadowsocks.ok() &&
                     parsedShadowsocks.fields.name == shadowsocksFields.name &&
                     parsedShadowsocks.fields.serverAddress == shadowsocksFields.serverAddress &&
                     parsedShadowsocks.fields.serverPort == shadowsocksFields.serverPort &&
                     parsedShadowsocks.fields.method == shadowsocksFields.method &&
                     parsedShadowsocks.fields.password == shadowsocksFields.password &&
                     parsedShadowsocks.fields.plugin == shadowsocksFields.plugin,
                 "SIP002 Shadowsocks fields and escaped v2ray-plugin options must round-trip");

    ShadowSocksShareFields shadowsocks2022;
    shadowsocks2022.serverAddress = QStringLiteral("ss2022.example");
    shadowsocks2022.serverPort = 443;
    shadowsocks2022.method = QStringLiteral("2022-blake3-aes-256-gcm");
    shadowsocks2022.password = QStringLiteral("YctPZ6U7xPPcU+gp3u+0tx/RizJN9K8y+uKlW2qjlI=");
    const auto builtShadowsocks2022 = BuildShadowSocksShareLink(shadowsocks2022);
    const auto parsedShadowsocks2022 = ParseShadowSocksShareLink(builtShadowsocks2022.link);
    ok &= expect(builtShadowsocks2022.ok() &&
                     builtShadowsocks2022.link.contains(QLatin1String("2022-blake3-aes-256-gcm:")) &&
                     builtShadowsocks2022.link.contains(QLatin1String("%2B")) &&
                     parsedShadowsocks2022.ok() &&
                     parsedShadowsocks2022.fields.password == shadowsocks2022.password,
                 "AEAD-2022 Shadowsocks userinfo must remain plain and percent encoded");

    const auto legacyShadowsocksPayload = QByteArrayLiteral(
        "chacha20-ietf-poly1305:pa:ss@word@legacy.example:1443").toBase64(
            QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals);
    const auto legacyShadowsocks = ParseShadowSocksShareLink(
        QStringLiteral("ss://") + QString::fromLatin1(legacyShadowsocksPayload) +
        QStringLiteral("#legacy%20node"));
    ok &= expect(legacyShadowsocks.ok() &&
                     legacyShadowsocks.fields.name == QStringLiteral("legacy node") &&
                     legacyShadowsocks.fields.serverAddress == QStringLiteral("legacy.example") &&
                     legacyShadowsocks.fields.serverPort == 1443 &&
                     legacyShadowsocks.fields.method == QStringLiteral("chacha20-ietf-poly1305") &&
                     legacyShadowsocks.fields.password == QStringLiteral("pa:ss@word"),
                 "deprecated whole-payload Shadowsocks base64 must split first colon and last at-sign");

    const auto plainSip002 = ParseShadowSocksShareLink(QStringLiteral(
        "ss://aes-256-gcm:pass%3Aword@[2001:db8::17]:8388/"
        "?plugin=simple-obfs%3Bobfs%3Dhttp#plain"));
    ok &= expect(plainSip002.ok() &&
                     plainSip002.fields.serverAddress == QStringLiteral("2001:db8::17") &&
                     plainSip002.fields.serverPort == 8388 &&
                     plainSip002.fields.method == QStringLiteral("aes-256-gcm") &&
                     plainSip002.fields.password == QStringLiteral("pass:word") &&
                     plainSip002.fields.plugin == QStringLiteral("obfs-local;obfs=http"),
                 "plain SIP002 userinfo, IPv6 server, and simple-obfs alias must parse without DNS");

    auto ipv6ShadowsocksFields = shadowsocksFields;
    ipv6ShadowsocksFields.serverAddress = QStringLiteral("2001:db8::18");
    ipv6ShadowsocksFields.plugin.clear();
    const auto builtIpv6Shadowsocks = BuildShadowSocksShareLink(ipv6ShadowsocksFields);
    const auto parsedIpv6Shadowsocks = ParseShadowSocksShareLink(builtIpv6Shadowsocks.link);
    ok &= expect(builtIpv6Shadowsocks.ok() && parsedIpv6Shadowsocks.ok() &&
                     parsedIpv6Shadowsocks.fields.serverAddress == ipv6ShadowsocksFields.serverAddress,
                 "SIP002 export must preserve an IPv6 server with URI brackets only at the format boundary");

    const auto clashV2RayPlugin = V2RayPluginFromClash(
        QStringLiteral("websocket"),
        QStringLiteral("cdn.example"),
        QStringLiteral(R"(/edge\a=b,c;d)"),
        true);
    ok &= expect(clashV2RayPlugin == QStringLiteral(
                     R"(v2ray-plugin;tls;host=cdn.example;path=/edge\\a\=b\,c\;d)") &&
                     V2RayPluginFromClash({}, {}, {}, false) == QStringLiteral("v2ray-plugin"),
                 "Clash v2ray-plugin values must be escaped for SIP003 without restoring Xray runtime");

    const auto invalidLegacyShadowsocks = ParseShadowSocksShareLink(QStringLiteral("ss://%%%"));
    const auto malformedLegacyShadowsocks = ParseShadowSocksShareLink(
        QStringLiteral("ss://") + QString::fromLatin1(QByteArrayLiteral("method:password").toBase64()));
    const auto invalidPortShadowsocks = ParseShadowSocksShareLink(
        QStringLiteral("ss://YWVzLTI1Ni1nY206cGFzcw@ss.example:70000"));
    ok &= expect(invalidLegacyShadowsocks.error == ShadowSocksShareError::InvalidBase64 &&
                     malformedLegacyShadowsocks.error == ShadowSocksShareError::InvalidSyntax &&
                     invalidPortShadowsocks.error == ShadowSocksShareError::InvalidPort,
                 "invalid Shadowsocks base64, syntax, and ports must fail explicitly");

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
