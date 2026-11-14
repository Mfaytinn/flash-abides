#!/usr/bin/env bash
# Recreate the two reproduction archives from the official Ubuntu packages listed in
# reproduction/SOURCES.md. Packages are downloaded from Launchpad, checked against pinned
# SHA-256 sums, unpacked with dpkg-deb, and packed deterministically inside a pinned
# Ubuntu image, so the archives come out byte-identical on every run.
set -euo pipefail
base=ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

if [[ ${1:-} != --inside ]]; then
    cd "$(dirname "$0")/.."
    out=${REPRO_INPUTS:-$PWD/dist/final-reproduction}
    mkdir -p "$out"
    "${CONTAINER:-docker}" run --rm --platform linux/amd64 --security-opt label=disable \
        -v "$PWD/reproduction:/repro:ro" -v "$out:/out" "$base" \
        bash /repro/make-archives.sh --inside
    (cd "$out" && sha256sum submission-final-toolchain.tar.gz submission-final-host-inputs.tar.gz)
    exit
fi

# Toolchain: GCC 16.2 and binutils 2.46 from Ubuntu 26.04.
gcc_debs=(
    "178a56bd3c1c1ad55c952fd6f8a7e1da069f73620baf5ddb8520c3db0310b7b2 gcc-16-x86-64-linux-gnu_16.2.0-3ubuntu1_amd64.deb"
    "00fa703491cd7ba79d1165acd7a16d3becdbea90140b57f48a334e931280af38 g++-16-x86-64-linux-gnu_16.2.0-3ubuntu1_amd64.deb"
    "86d85343f8e91821a517ce382d8324e93f318ac04c9858a4da20dd3adbaab19a cpp-16-x86-64-linux-gnu_16.2.0-3ubuntu1_amd64.deb"
    "e71965c2243763638804db6fbcc46e0aeda8d1df139d6c40e5c1f68acd940ef8 libgcc-16-dev_16.2.0-3ubuntu1_amd64.deb"
    "b1bcab2cc6ccdc2eb25521e8c229432b2bea0ea674118f3eb4a69e9550162dda libstdc++-16-dev_16.2.0-3ubuntu1_amd64.deb"
)
binutils_debs=(
    "91fa094c31783f7ccf0f0df8ed06447655c40476ba61fc95dbd245a313f8171e binutils-x86-64-linux-gnu_2.46-3ubuntu2_amd64.deb"
    "1376a5e49c8dc2ba938fadc15fce40dd01c7fbf1adf2d506a2a7cf7148e347c8 libbinutils_2.46-3ubuntu2_amd64.deb"
    "34b1757ce619720541c7e1e8d8f3aa506cb327731374e284ffb6ec872a2c566f libctf0_2.46-3ubuntu2_amd64.deb"
    "5ca038ae110ddf65e190b9fe88822592abd45cecba5001f3e4ce7692ca1f31be libctf-nobfd0_2.46-3ubuntu2_amd64.deb"
    "05541ea1edb19e54bea297a21a9c33d0b99ca2cfe2e86d4a67335e6640667cfc libsframe3_2.46-3ubuntu2_amd64.deb"
)
# Host inputs: C/C++ headers and static runtime libraries from Ubuntu 24.04.
host_debs=(
    "ee5633e863e19c3381ed97842ce35ed32ede96a3d1ae4e94c051d3036fe21347 libstdc++-13-dev_13.3.0-6ubuntu2~24.04.1_amd64.deb"
    "cd689db2691edaa10f37329307292796bb599e722e0505c79e14caaa1fe9a93a libgcc-13-dev_13.3.0-6ubuntu2~24.04.1_amd64.deb"
    "e13d5fcc1b2a86f75bca8e0026a8e39f24fe97ca86e92be79991f8697eb1306f libc6-dev_2.39-0ubuntu8.9_amd64.deb"
    "1a7bb04132d01cd14bea3d615e5861096ca1f70c4659902eb14b36c21993cadc linux-libc-dev_6.8.0-146.146_amd64.deb"
)

apt-get update -qq >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends curl ca-certificates >/dev/null
mkdir -p /work/debs /work/toolchain/toolchains/gcc /work/toolchain/toolchains/binutils /work/host
cd /work/debs
fetch() {  # fetch DIR "SHA FILE"...
    local dir=$1; shift
    for entry in "$@"; do
        local sum=${entry%% *} file=${entry#* }
        curl -fsSL --retry 3 -o "$file" \
            "https://launchpad.net/ubuntu/+archive/primary/+files/${file//\~/%7E}"
        echo "$sum  $file" | sha256sum --check --strict --quiet
        dpkg-deb -x "$file" "$dir"
    done
}
fetch /work/toolchain/toolchains/gcc "${gcc_debs[@]}"
fetch /work/toolchain/toolchains/binutils "${binutils_debs[@]}"
fetch /work/host "${host_debs[@]}"
[[ $(ls /work/host) == usr ]] || { echo 'host packages unpacked outside /usr' >&2; exit 1; }
# The binary links the GCC 13 C++ runtime from the host inputs, not GCC 16's.
rm /work/toolchain/toolchains/gcc/usr/lib/gcc/x86_64-linux-gnu/16/libstdc++.a
# GCC's driver looks for unprefixed `as` and `ld` on COMPILER_PATH.
ln -s x86_64-linux-gnu-as /work/toolchain/toolchains/binutils/usr/bin/as
ln -s x86_64-linux-gnu-ld.bfd /work/toolchain/toolchains/binutils/usr/bin/ld

pack() {  # pack DIR ARCHIVE TOP-LEVEL-ENTRY
    tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=gnu \
        -C "$1" -cf - "$3" | gzip -n -9 >"/out/$2"
}
pack /work/toolchain submission-final-toolchain.tar.gz toolchains
pack /work/host submission-final-host-inputs.tar.gz usr
