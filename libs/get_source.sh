#!/bin/bash
set -e

source libs/env_deploy.sh
ENV_NEKORAY=1
source libs/get_source_env.sh
pushd ..

####

git -C nekoray submodule update --init --recursive \
  third_party/routefluent-sing-box \
  third_party/libneko

####

if [ ! -d "sing-quic" ]; then
  git clone --no-checkout https://github.com/MatsuriDayo/sing-quic.git
fi
pushd sing-quic
git checkout "$COMMIT_SING_QUIC"

popd

####

popd
