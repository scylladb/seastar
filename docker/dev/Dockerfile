# syntax=docker/dockerfile:1

FROM ubuntu:plucky

RUN --mount=type=bind,source=./install-dependencies.sh,target=/install-dependencies.sh \
    apt-get update && apt-get install -y \
    curl \
    gnupg \
    build-essential \
    clang-20 \
    clang-19 \
    clang-tools-20 \
    gcc-15 \
    g++-15 \
    gcc-14 \
    g++-14 \
    pandoc \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-15 15 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-15 15 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 14 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 14 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-20 20 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-20 20 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-19 19 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-19 19 \
    && bash ./install-dependencies.sh \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash"]
