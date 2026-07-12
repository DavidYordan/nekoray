## Build nekobox_core

The core build path is RouteFluent patched sing-box only.

### Source Layout

```text
nekoray/
  go/cmd/nekobox_core/
  third_party/routefluent-sing-box/
```

`libs/get_source.sh` initializes the RouteFluent submodule. `libs/build_go.sh` then runs
`third_party/routefluent-sing-box/build_routefluent_sing_box.py` to prepare the patched
sing-box source tree used by `go/cmd/nekobox_core/go.mod`.

### Build

```bash
bash libs/get_source.sh
GOOS=windows GOARCH=amd64 bash libs/build_go.sh
```

The generated package includes `routefluent-sing-box-manifest.json`, which must report:

- `version_name`: `1.13.12-routefluent-anytls-client.7`
- `patch_id`: `routefluent-anytls-client-dns-resolver-group-check-v1`
- `features`: `anytls_outbound_client_field`, `routefluent_dns_resolver_group`, `routefluent_dns_check_start_validation`

Supported `GOOS` and `GOARCH` combinations are defined in `libs/build_go.sh`.
