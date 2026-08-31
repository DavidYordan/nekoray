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
        AmbiguousServerAddress,
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

    struct SocksUserInfoResult {
        QString username;
        QString password;
        bool decodedLegacyBase64 = false;
    };

    enum class V2RayNVmessError {
        None,
        NotV2RayN,
        InvalidBase64,
        InvalidJson,
        MissingRequiredField,
        InvalidPort,
    };

    struct V2RayNVmessFields {
        QString name;
        QString serverAddress;
        int serverPort = 0;
        QString uuid;
        int alterId = 0;
        QString network = QStringLiteral("tcp");
        QString host;
        QString path;
        QString headerType;
        QString security = QStringLiteral("auto");
        QString tls;
        QString sni;
        QString alpn;
        QString fingerprint;
        bool allowInsecure = false;
    };

    struct V2RayNVmessParseResult {
        V2RayNVmessFields fields;
        V2RayNVmessError error = V2RayNVmessError::NotV2RayN;

        [[nodiscard]] bool ok() const { return error == V2RayNVmessError::None; }
    };

    struct V2RayNVmessBuildResult {
        QString link;
        V2RayNVmessError error = V2RayNVmessError::MissingRequiredField;

        [[nodiscard]] bool ok() const {
            return error == V2RayNVmessError::None && !link.isEmpty();
        }
    };

    enum class ShadowSocksShareError {
        None,
        NotShadowsocks,
        InvalidBase64,
        InvalidSyntax,
        MissingRequiredField,
        InvalidPort,
    };

    struct ShadowSocksShareFields {
        QString name;
        QString serverAddress;
        int serverPort = 0;
        QString method;
        QString password;
        QString plugin;
    };

    struct ShadowSocksShareParseResult {
        ShadowSocksShareFields fields;
        ShadowSocksShareError error = ShadowSocksShareError::NotShadowsocks;

        [[nodiscard]] bool ok() const { return error == ShadowSocksShareError::None; }
    };

    struct ShadowSocksShareBuildResult {
        QString link;
        ShadowSocksShareError error = ShadowSocksShareError::MissingRequiredField;

        [[nodiscard]] bool ok() const {
            return error == ShadowSocksShareError::None && !link.isEmpty();
        }
    };

    // Native links are already FullyEncoded. A literal '#' can therefore only
    // begin the URI fragment; percent-encoded data such as "%23" is preserved.
    [[nodiscard]] ShareFormatResult ShareLinkWithoutRemark(const QString& nativeLink);

    // v2rayN-compatible SOCKS links may store the complete `user:password`
    // pair as standard base64 in the URI username field. Explicit URI
    // passwords and ambiguous/invalid inputs are preserved verbatim.
    [[nodiscard]] SocksUserInfoResult DecodeLegacySocksBase64UserInfo(
        const QString& username,
        const QString& password);

    // v2rayN VMess links are standard-base64 encoded JSON. The helper owns
    // only format conversion: it does not select a runtime core or access DNS.
    // Internal HTTP transport is normalized to/from the schema's `h2` value.
    [[nodiscard]] V2RayNVmessParseResult ParseV2RayNVmessLink(const QString& link);
    [[nodiscard]] V2RayNVmessBuildResult BuildV2RayNVmessLink(
        const V2RayNVmessFields& fields);

    // Supports SIP002 plus the deprecated whole-payload base64 format for
    // import compatibility. Export always uses SIP002. Plugin text remains a
    // SIP003 option string; parsing it does not select an external core.
    [[nodiscard]] ShadowSocksShareParseResult ParseShadowSocksShareLink(
        const QString& link);
    [[nodiscard]] ShadowSocksShareBuildResult BuildShadowSocksShareLink(
        const ShadowSocksShareFields& fields);

    // Clash expresses v2ray-plugin options as YAML fields while sing-box's
    // internal SIP003 implementation consumes the escaped semicolon string.
    [[nodiscard]] QString V2RayPluginFromClash(
        const QString& mode,
        const QString& host,
        const QString& path,
        bool tls);

    // This deliberately narrow credential-list format preserves the server
    // field verbatim and never performs DNS or percent-encodes delimiters.
    // Inputs that cannot be represented unambiguously are rejected rather than
    // silently changed.
    [[nodiscard]] ShareFormatResult ServerPortUserPass(
        CredentialProxyKind kind,
        const QString& serverAddress,
        int serverPort,
        const QString& username,
        const QString& password,
        bool transportUsesTls);

    [[nodiscard]] QString ShareFormatErrorDescription(ShareFormatError error);
} // namespace NekoGui_fmt
