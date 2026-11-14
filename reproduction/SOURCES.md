# Toolchain sources

The two reproduction archives contain unmodified files from official Ubuntu packages. This
page lists those packages, so anyone can check where each file came from and get its source
code. Launchpad keeps every published Ubuntu package version, binary and source,
permanently.

`reproduction/make-archives.sh` downloads these packages from Launchpad, checks the SHA-256
sums below, and recreates both archives byte for byte:

| Archive | SHA-256 |
|---|---|
| `submission-final-toolchain.tar.gz` | `28f23179ce5d4081fb4154f7ec2e43ef9d25602c22ac9c843435d9c2a54a4d01` |
| `submission-final-host-inputs.tar.gz` | `82eb71f771a53a4b7c2e24620d52aa78ae297b30e394faa8176c10dcb03a68f6` |

## `submission-final-toolchain.tar.gz`

The compiler and assembler: GCC 16.2 and binutils 2.46 from Ubuntu 26.04 ("resolute"),
unpacked with `dpkg-deb -x` into `toolchains/gcc` and `toolchains/binutils`.

| Package | Version | SHA-256 of the `.deb` |
|---|---|---|
| `gcc-16-x86-64-linux-gnu` | 16.2.0-3ubuntu1 | `178a56bd3c1c1ad55c952fd6f8a7e1da069f73620baf5ddb8520c3db0310b7b2` |
| `g++-16-x86-64-linux-gnu` | 16.2.0-3ubuntu1 | `00fa703491cd7ba79d1165acd7a16d3becdbea90140b57f48a334e931280af38` |
| `cpp-16-x86-64-linux-gnu` | 16.2.0-3ubuntu1 | `86d85343f8e91821a517ce382d8324e93f318ac04c9858a4da20dd3adbaab19a` |
| `libgcc-16-dev` | 16.2.0-3ubuntu1 | `e71965c2243763638804db6fbcc46e0aeda8d1df139d6c40e5c1f68acd940ef8` |
| `libstdc++-16-dev` | 16.2.0-3ubuntu1 | `b1bcab2cc6ccdc2eb25521e8c229432b2bea0ea674118f3eb4a69e9550162dda` |
| `binutils-x86-64-linux-gnu` | 2.46-3ubuntu2 | `91fa094c31783f7ccf0f0df8ed06447655c40476ba61fc95dbd245a313f8171e` |
| `libbinutils` | 2.46-3ubuntu2 | `1376a5e49c8dc2ba938fadc15fce40dd01c7fbf1adf2d506a2a7cf7148e347c8` |
| `libctf0` | 2.46-3ubuntu2 | `34b1757ce619720541c7e1e8d8f3aa506cb327731374e284ffb6ec872a2c566f` |
| `libctf-nobfd0` | 2.46-3ubuntu2 | `5ca038ae110ddf65e190b9fe88822592abd45cecba5001f3e4ce7692ca1f31be` |
| `libsframe3` | 2.46-3ubuntu2 | `05541ea1edb19e54bea297a21a9c33d0b99ca2cfe2e86d4a67335e6640667cfc` |

`libstdc++.a` from `libstdc++-16-dev` is removed; the binary links the GCC 13 C++ runtime
below instead. Two symlinks are added, `as` and `ld`, pointing to `x86_64-linux-gnu-as` and
`x86_64-linux-gnu-ld.bfd`.

## `submission-final-host-inputs.tar.gz`

The C and C++ headers and static runtime libraries the binary is compiled and linked
against, from Ubuntu 24.04, unpacked with `dpkg-deb -x` under `/usr`:

| Package | Version | Provides | SHA-256 of the `.deb` |
|---|---|---|---|
| `libstdc++-13-dev` | 13.3.0-6ubuntu2~24.04.1 | C++ headers and `libstdc++.a` | `ee5633e863e19c3381ed97842ce35ed32ede96a3d1ae4e94c051d3036fe21347` |
| `libgcc-13-dev` | 13.3.0-6ubuntu2~24.04.1 | GCC 13 support objects and libraries | `cd689db2691edaa10f37329307292796bb599e722e0505c79e14caaa1fe9a93a` |
| `libc6-dev` | 2.39-0ubuntu8.9 | C headers, `libc.a`, `libm.a`, start files | `e13d5fcc1b2a86f75bca8e0026a8e39f24fe97ca86e92be79991f8697eb1306f` |
| `linux-libc-dev` | 6.8.0-146.146 | Linux kernel headers | `1a7bb04132d01cd14bea3d615e5861096ca1f70c4659902eb14b36c21993cadc` |

## Source code and licenses

| Ubuntu source package | Version | License | Source |
|---|---|---|---|
| `gcc-16` | 16.2.0-3ubuntu1 | GPL-3.0-or-later; runtime libraries with the GCC Runtime Library Exception | https://launchpad.net/ubuntu/+source/gcc-16/16.2.0-3ubuntu1 |
| `binutils` | 2.46-3ubuntu2 | GPL-3.0-or-later | https://launchpad.net/ubuntu/+source/binutils/2.46-3ubuntu2 |
| `gcc-13` | 13.3.0-6ubuntu2~24.04.1 | GPL-3.0-or-later; runtime libraries with the GCC Runtime Library Exception | https://launchpad.net/ubuntu/+source/gcc-13/13.3.0-6ubuntu2~24.04.1 |
| `glibc` | 2.39-0ubuntu8.9 | LGPL-2.1-or-later | https://launchpad.net/ubuntu/+source/glibc/2.39-0ubuntu8.9 |
| `linux` | 6.8.0-146.146 | GPL-2.0 with the Linux syscall note | https://launchpad.net/ubuntu/+source/linux/6.8.0-146.146 |

The builder image (`reproduction/Dockerfile`) also installs a few runtime libraries for the
compiler (GMP, MPFR, MPC, ISL, zlib, zstd) from the Ubuntu 24.04 archive at build time;
they are not redistributed here.
