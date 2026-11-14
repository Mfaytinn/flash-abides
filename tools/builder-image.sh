#!/usr/bin/env bash
# Build the amd64 builder image from the two toolchain archives, unless it already exists.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
container=${CONTAINER:-docker}
image=${BUILDER_IMAGE:-localhost/track3-final-builder:local}
if "$container" image inspect "$image" >/dev/null 2>&1; then exit 0; fi
inputs=${REPRO_INPUTS:-$root/dist/final-reproduction}
(cd "$inputs" && printf '%s\n' \
    '28f23179ce5d4081fb4154f7ec2e43ef9d25602c22ac9c843435d9c2a54a4d01  submission-final-toolchain.tar.gz' \
    '82eb71f771a53a4b7c2e24620d52aa78ae297b30e394faa8176c10dcb03a68f6  submission-final-host-inputs.tar.gz' | sha256sum -c -)
"$container" build --platform linux/amd64 -f "$root/reproduction/Dockerfile" -t "$image" "$inputs"
