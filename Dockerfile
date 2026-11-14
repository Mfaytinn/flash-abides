# Track-3 submission image for the lean engine: one statically linked binary, nothing else.
#
# Build the binary first (from the repository root):
#   cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release -DT3_STATIC=ON
#   cmake --build build-static -j
# then build the image with the build directory as context:
#   docker build --platform=linux/amd64 -f Dockerfile -t <image> build-static
#
# The harness passes the verb as the first argument (`simulate ...` or `simulate-batch ...`);
# the binary dispatches on it.
FROM scratch
LABEL qfbench2.interface_version="2.0"
# Every Track-3 unit attaches a GPU. Under gVisor (runsc) a GPU container without declared driver
# capabilities never starts (exit 125, measured by the organizers: public throughput/timer.py);
# their launcher passes this variable, and an explicit -e overrides it. The engine never opens
# the device.
ENV NVIDIA_DRIVER_CAPABILITIES=compute
# glibc 2.35 and later: request huge pages for the malloc heap via madvise.
# A kernel without huge-page support ignores the request.
ENV GLIBC_TUNABLES=glibc.malloc.hugetlb=1
COPY simulate /simulate
ENTRYPOINT ["/simulate"]
