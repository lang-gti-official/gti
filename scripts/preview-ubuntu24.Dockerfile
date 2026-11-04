FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
ENV PATH="/usr/lib/llvm-20/bin:${PATH}"
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates ccache clang-20 cmake curl g++-14 gcc-14 \
      git gh libclang-20-dev libcurl4-openssl-dev llvm-20-dev ninja-build \
      pkg-config python3 \
    && rm -rf /var/lib/apt/lists/*
# Build this recipe once, then pass its sha256 image ID to preview_remote.py.
# The remote runner owns source/build trees; each step uses docker run --rm.
