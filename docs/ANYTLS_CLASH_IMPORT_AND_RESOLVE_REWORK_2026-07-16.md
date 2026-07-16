# AnyTLS Clash Import And Resolve Rework

Date: 2026-07-16

## Goals

1. Clash subscriptions importing `type: anytls` default to Mihomo client identity when no explicit AnyTLS client field is present.
2. Plain links, including `anytls://`, keep native sing-box behavior unless the link explicitly carries `anytls_client_mode` or `anytls_client_value`.
3. AnyTLS client identity remains editable per profile.
4. Clash-provided DoH used to resolve proxy server names is visible and editable per profile.
5. Resolve-domain no longer directly mutates profiles; it resolves first, then offers Replace or Copy as new.

## Current State

- `sub/GroupUpdater.cpp` handles Clash YAML import.
- `fmt/Link2Bean.cpp` handles plain links; AnyTLS links default to `native`.
- `ui/edit/edit_anytls.ui` already exposes `Client Identity` and `Custom Client`.
- `ui/edit/dialog_edit_profile.ui` already exposes `Server Resolver`, `DoH Upstreams`, and `Allow Local Fallback`.
- `fmt/AbstractBean::ResolveDomainToIP` still uses `QHostInfo` and directly replaces `serverAddress`; the menu action calls this legacy path.

## Clash Field Support

Supported and mapped:

- Common: `name`, `server`, `port`.
- Provider resolver: per-proxy `server-resolver` / `server_resolver`; otherwise top-level Clash `dns.proxy-server-nameserver` or `dns.nameserver`.
- Shadowsocks: `cipher`, `password`, `plugin`, `plugin-opts`, `udp-over-tcp`, `smux.enabled`.
- SOCKS/HTTP: `username`, `password`, `tls`, `skip-cert-verify`.
- Trojan/VLESS: `password` or `uuid`, `flow`, `network`, `sni` / `servername`, `alpn`, `client-fingerprint`, `packet-addr`, `smux.enabled`, `ws-opts`, `grpc-opts`, `reality-opts`.
- VMess: `uuid`, `alterId`, `cipher`, `network`, `sni` / `servername`, `alpn`, `tls`, `skip-cert-verify`, `client-fingerprint`, `xudp`, `packet-addr`, `smux.enabled`, `ws-opts`, `grpc-opts`, `h2-opts`, `http-opts`.
- Hysteria2: `ports`, `skip-cert-verify`, `ca-str`, `sni`, `obfs-password`, `password`, `up`, `down`.
- TUIC: `uuid`, `password`, `heartbeat-interval`, `udp-relay-mode`, `congestion-controller`, `disable-sni`, `reduce-rtt`, `skip-cert-verify`, `alpn`, `ca-str`, `sni`, `udp-over-stream`, `ip`.
- AnyTLS: `password`, `skip-cert-verify` / `insecure`, `disable-sni` / `disable_sni`, `ca-str` / `certificate`, `sni` / `servername` / `server_name`, `alpn`, `client-fingerprint` / `fingerprint` / `utls_fingerprint`, idle-session fields, `anytls-client-mode` / `anytls_client_mode`, `anytls-client-value` / `anytls_client_value`, `client`, `reality-opts`.

Discarded by design:

- Clash proxy groups, rule providers, rules, global proxy mode, Clash DNS routing policy, fake-ip policy, health-check/url-test policy, load-balance/select semantics, and dashboard/runtime-only Clash settings.
- These are not per-line outbound fields. Importing them into this project would blur subscription import with full Clash runtime emulation and can conflict with NekoRay's own routing/profile model.

Evaluation:

- Keeping protocol fields, TLS/transport fields, and provider server resolver is valuable because they affect whether a single outbound can connect.
- Dropping Clash policy/rule/group runtime state is reasonable for this project: NekoRay already has its own group, route, and test model. Mapping Clash groups would be lossy and misleading unless implemented as a separate Clash-runtime import mode.

## Resolve-Domain Design

Resolver choices:

- Auto: use profile provider DoH when present; otherwise use system resolver.
- Provider DoH: use the profile's `Server Resolver` DoH upstreams.
- Remote DNS: use current routing profile's Remote DNS when it is HTTPS DoH.
- Direct DNS: use current routing profile's Direct DNS when it is HTTPS DoH.
- System: use Qt/OS resolver.

Recommendation:

- Default Auto.
- For Clash-imported nodes with provider DoH, Auto should resolve through provider DoH so server-domain answers match the subscription provider's intent.
- Do not silently replace the node address. Show results first, then let the user choose Replace or Copy as new.

## Implementation Checklist

- [x] Clash AnyTLS default client identity: `mihomo` when no explicit client fields exist.
- [x] Plain AnyTLS links keep native sing-box default.
- [x] AnyTLS client mode UI is present.
- [x] Provider DoH UI is present.
- [x] Improve labels/tooltips so client and provider DoH semantics are obvious.
- [x] Replace legacy resolve-domain action with preview dialog and Replace / Copy as new actions.
- [x] Build and run targeted tests.
