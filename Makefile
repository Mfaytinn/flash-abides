.DEFAULT_GOAL := build
CMAKE ?= cmake
JOBS ?= 4
CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release -DT3_STATIC=ON
CLANG_FORMAT ?= $(shell command -v clang-format-22 2>/dev/null || command -v clang-format 2>/dev/null)
FORMAT_SOURCES := $(sort $(wildcard src/*.cpp src/*.inc) $(shell find include -type f -name '*.hpp'))

CONTAINER ?= docker
BUILDER_IMAGE ?= localhost/track3-final-builder:local
FINAL_SUBMISSION ?= submission-final.zip

.PHONY: all build final-binary exact-binary submission-final format format-check check-clang-format
all: build

# Native builds target x86-64-v3; use an x86-64 host.
build:
	$(CMAKE) -S . -B build $(CMAKE_FLAGS)
	$(CMAKE) --build build --target simulate --parallel $(JOBS)

# Rebuilds the submitted configuration (GCC 16, LTO, PGO) in a container on x86-64.
final-binary:
	CONTAINER="$(CONTAINER)" BUILDER_IMAGE="$(BUILDER_IMAGE)" JOBS="$(JOBS)" bash tools/build-final.sh

# Rebuilds the submitted binary byte for byte from reproduction/exact/.
exact-binary:
	CONTAINER="$(CONTAINER)" BUILDER_IMAGE="$(BUILDER_IMAGE)" bash tools/build-exact.sh

# Reuse an existing signed claim only after checking the referenced image's binary.
submission-final: exact-binary
	python3 tools/package-final.py --container "$(CONTAINER)" --submission "$(FINAL_SUBMISSION)" --binary build/exact/simulate

# Pin the patch version too: formatting output can change between LLVM releases.
check-clang-format:
	@"$(CLANG_FORMAT)" --version | grep -Eq '(^|[[:space:]])version 22\.1\.8([[:space:]]|$$)' || { echo 'Install clang-format 22.1.8 (or set CLANG_FORMAT to its executable).' >&2; exit 1; }

format: check-clang-format
	"$(CLANG_FORMAT)" --style=file -i $(FORMAT_SOURCES)

format-check: check-clang-format
	"$(CLANG_FORMAT)" --style=file --dry-run --Werror $(FORMAT_SOURCES)
