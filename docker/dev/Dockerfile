# syntax=docker/dockerfile:1

FROM ubuntu:resolute

RUN --mount=type=bind,source=./install-dependencies.sh,target=/install-dependencies.sh \
    apt-get update && apt-get install -y \
    curl \
    gnupg \
    build-essential \
    clang-22 \
    clang-21 \
    clang-tools-22 \
    gcc-16 \
    g++-16 \
    gcc-15 \
    g++-15 \
    pandoc \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 16 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-16 16 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-15 15 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-15 15 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-22 22 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-22 22 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-21 21 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 21 \
    && bash ./install-dependencies.sh \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash"]
