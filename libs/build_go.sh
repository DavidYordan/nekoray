#!/bin/bash
set -e

source libs/env_deploy.sh
ROUTEFLUENT_SING_BOX_VERSION="1.13.12-routefluent-anytls-client.7"
GO_BUILD_TAGS="with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls"
ROUTEFLUENT_SING_BOX_TAGS="${GO_BUILD_TAGS//,/ }"
ROUTEFLUENT_SING_BOX_MANIFEST="$PWD/build-routefluent-sing-box/sing-box-$GOOS-$GOARCH.routefluent-anytls-client.json"
ROUTEFLUENT_SING_BOX_OUTPUT="$PWD/build-routefluent-sing-box/sing-box-$GOOS-$GOARCH"
[ "$GOOS" == "windows" ] && ROUTEFLUENT_SING_BOX_OUTPUT="$ROUTEFLUENT_SING_BOX_OUTPUT.exe"
[ "$GOOS" == "windows" ] && [ "$GOARCH" == "amd64" ] && DEST=$DEPLOYMENT/windows64 || true
[ "$GOOS" == "windows" ] && [ "$GOARCH" == "arm64" ] && DEST=$DEPLOYMENT/windows-arm64 || true
[ "$GOOS" == "linux" ] && [ "$GOARCH" == "amd64" ] && DEST=$DEPLOYMENT/linux64 || true
[ "$GOOS" == "linux" ] && [ "$GOARCH" == "arm64" ] && DEST=$DEPLOYMENT/linux-arm64 || true
if [ -z $DEST ]; then
  echo "Please set GOOS GOARCH"
  exit 1
fi
rm -rf $DEST
mkdir -p $DEST

export CGO_ENABLED=0

#### RouteFluent patched sing-box source ####
python3 third_party/routefluent-sing-box/build_routefluent_sing_box.py \
  --goos "$GOOS" \
  --goarch "$GOARCH" \
  --tags "$ROUTEFLUENT_SING_BOX_TAGS" \
  --output "$ROUTEFLUENT_SING_BOX_OUTPUT" \
  --manifest "$ROUTEFLUENT_SING_BOX_MANIFEST"

#### Go: updater ####
pushd go/cmd/updater
[ "$GOOS" == "darwin" ] || go build -o $DEST -trimpath -ldflags "-w -s"
[ "$GOOS" == "linux" ] && mv $DEST/updater $DEST/launcher || true
popd

#### Go: nekobox_core ####
pushd go/cmd/nekobox_core
go build -v -o $DEST -trimpath -ldflags "-w -s -X github.com/matsuridayo/libneko/neko_common.Version_neko=$version_standalone -X github.com/sagernet/sing-box/constant.Version=$ROUTEFLUENT_SING_BOX_VERSION" -tags "$GO_BUILD_TAGS"
popd
cp "$ROUTEFLUENT_SING_BOX_MANIFEST" "$DEST/routefluent-sing-box-manifest.json"
