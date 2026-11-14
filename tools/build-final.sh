#!/usr/bin/env bash
# Build with the preserved x86-64 compiler in a container; never the host C++ compiler.
set -euo pipefail
workspace=/home/verisalis003_gmail_com/compiler-pgo-gokturk-20261013
if [[ ${1:-} == --inside ]]; then
    cd "$workspace"
    gcc=$workspace/toolchains/gcc/usr
    export COMPILER_PATH=$workspace/toolchains/binutils/usr/bin
    export PATH=$COMPILER_PATH:$PATH
    flags="-O3 -g -DNDEBUG -fno-fast-math -ffp-contract=off -fno-pie -fcf-protection=none -fno-stack-protector -nostdinc++ -isystem /usr/include/c++/13 -isystem /usr/include/x86_64-linux-gnu/c++/13 -isystem /usr/include/c++/13/backward -B$COMPILER_PATH/"
    # The submitted binary was linked with -flto=4. LTO=0 turns LTO off.
    lto=
    if [[ ${LTO:-4} != 0 ]]; then lto="-flto=${LTO:-4}"; fi
    # Formatting changes invalidate source locations in the historical PGO data.
    # Keep diagnostics visible, but do not fail the build on them.
    profile="-fprofile-use=$workspace/gcc/raw -fprofile-correction -Wno-error=missing-profile -Wno-error=coverage-mismatch"
    cmake -S "$workspace/engine" -B "$workspace/gcc/build" \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
        -DCMAKE_CXX_COMPILER="$gcc/bin/x86_64-linux-gnu-g++-16" \
        -DCMAKE_AR="$gcc/bin/x86_64-linux-gnu-gcc-ar-16" \
        -DCMAKE_RANLIB="$gcc/bin/x86_64-linux-gnu-gcc-ranlib-16" \
        -DCMAKE_BUILD_TYPE=Release -DT3_STATIC=ON \
        "-DCMAKE_CXX_FLAGS_RELEASE=$flags $lto $profile" \
        "-DCMAKE_EXE_LINKER_FLAGS=-no-pie -L$gcc/lib/gcc/x86_64-linux-gnu/16 -L/usr/lib/gcc/x86_64-linux-gnu/13 $lto $profile"
    cmake --build "$workspace/gcc/build" --target simulate --parallel "${JOBS:-4}"
    exit
fi
cd "$(dirname "$0")/.."
root=$PWD
container=${CONTAINER:-docker}
image=${BUILDER_IMAGE:-localhost/track3-final-builder:local}
mkdir -p build/cross-final/raw build/cross-final/build
CONTAINER="$container" BUILDER_IMAGE="$image" bash tools/builder-image.sh
tar -xzf reproduction/gcc-profiles.tar.gz -C build/cross-final/raw
"$container" run --rm --platform linux/amd64 --network none --security-opt label=disable \
    -e JOBS="${JOBS:-4}" -e LTO="${LTO:-4}" \
    -v "$root:$workspace/engine:ro" -v "$root/build/cross-final:$workspace/gcc:rw" \
    --entrypoint /bin/bash "$image" "$workspace/engine/tools/build-final.sh" --inside
sha256sum build/cross-final/build/simulate
