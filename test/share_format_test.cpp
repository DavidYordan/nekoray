#include "fmt/ShareFormats.hpp"

#include <QCoreApplication>
#include <QDebug>

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

    const auto socks = IpPortUserPass(
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
    ok &= expect(IpPortUserPass(
                     CredentialProxyKind::Http,
                     QStringLiteral("192.0.2.10"),
                     8080,
                     QStringLiteral("user"),
                     QStringLiteral("pass"),
                     false)
                     .ok(),
                 "plain HTTP credentials must be representable");
    ok &= expect(IpPortUserPass(
                     CredentialProxyKind::Http,
                     QStringLiteral("192.0.2.10"),
                     443,
                     QStringLiteral("user"),
                     QStringLiteral("pass"),
                     true)
                         .error == ShareFormatError::TlsWouldBeLost,
                 "TLS HTTP must be rejected because the flat format loses TLS");
    ok &= expect(IpPortUserPass(
                     CredentialProxyKind::Unsupported,
                     QStringLiteral("192.0.2.10"),
                     443,
                     QStringLiteral("user"),
                     QStringLiteral("pass"),
                     false)
                         .error == ShareFormatError::UnsupportedProtocol,
                 "unrelated protocols must not be flattened as proxy credentials");
    for (const auto& address : {QStringLiteral("proxy.example"), QStringLiteral("2001:db8::1")}) {
        ok &= expect(IpPortUserPass(
                         CredentialProxyKind::Socks5,
                         address,
                         1080,
                         QStringLiteral("user"),
                         QStringLiteral("pass"),
                         false)
                             .error == ShareFormatError::AddressIsNotLiteralIpv4,
                     "domains and IPv6 must not trigger DNS or ambiguous flattening");
    }
    for (const auto port : {0, 65536}) {
        ok &= expect(IpPortUserPass(
                         CredentialProxyKind::Socks5,
                         QStringLiteral("192.0.2.10"),
                         port,
                         QStringLiteral("user"),
                         QStringLiteral("pass"),
                         false)
                             .error == ShareFormatError::InvalidPort,
                     "invalid ports must be rejected");
    }
    ok &= expect(IpPortUserPass(
                     CredentialProxyKind::Socks5,
                     QStringLiteral("192.0.2.10"),
                     1080,
                     {},
                     QStringLiteral("pass"),
                     false)
                         .error == ShareFormatError::MissingCredentials,
                 "missing credentials must be rejected");
    for (const auto& value : {QStringLiteral("user:name"), QStringLiteral("line\nbreak")}) {
        ok &= expect(IpPortUserPass(
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
