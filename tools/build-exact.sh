#!/usr/bin/env bash
# Rebuild the submitted Final binary byte for byte from reproduction/exact/: the source in
# its original line layout, built with the original toolchain and PGO profiles.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
container=${CONTAINER:-docker}
image=${BUILDER_IMAGE:-localhost/track3-final-builder:local}
CONTAINER="$container" BUILDER_IMAGE="$image" bash tools/builder-image.sh
mkdir -p build/exact
# The image's working directory is the original build workspace; rebuild.py restores the
# source there, compiles it and refuses to finish unless the result is byte-identical.
"$container" run --rm --platform linux/amd64 --network none --security-opt label=disable \
    -v "$root/reproduction:/repro:ro" -v "$root/build/exact:/result" \
    --entrypoint /bin/bash "$image" -euc \
    'mkdir -p gcc/raw
     tar --no-same-owner -xzf /repro/gcc-profiles.tar.gz -C gcc/raw
     python3 /repro/exact/rebuild.py --metadata /repro/exact/manifest.json
     cp gcc/build/simulate /result/simulate'
echo 'a0ffe98228ae20d8915843516ca970a3fb39287ccaa83fd83f3613be96351369  build/exact/simulate' |
    sha256sum --check --strict
