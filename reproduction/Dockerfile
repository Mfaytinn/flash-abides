# Build-time environment only; Dockerfile at the repository root is the runtime image.
FROM --platform=linux/amd64 ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make python3 libisl23 libmpc3 libmpfr6 libgmp10 libjansson4 zlib1g libzstd1 \
    && rm -rf /var/lib/apt/lists/*
ADD submission-final-host-inputs.tar.gz /
WORKDIR /home/verisalis003_gmail_com/compiler-pgo-gokturk-20261013
ADD submission-final-toolchain.tar.gz ./
RUN ln -s x86_64-linux-gnu-ar toolchains/binutils/usr/bin/ar \
    && ln -s x86_64-linux-gnu-ranlib toolchains/binutils/usr/bin/ranlib
ENV LD_LIBRARY_PATH=/home/verisalis003_gmail_com/compiler-pgo-gokturk-20261013/toolchains/binutils/usr/lib/x86_64-linux-gnu
