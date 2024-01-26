FROM ubuntu:mantic
RUN apt-get -y update
RUN apt-get -y install gnupg curl
RUN echo "deb http://apt.llvm.org/mantic/ llvm-toolchain-mantic-17 main" \
    >> /etc/apt/sources.list.d/llvm.list
RUN echo "deb http://apt.llvm.org/mantic/ llvm-toolchain-mantic-18 main" \
    >> /etc/apt/sources.list.d/llvm.list
RUN curl -sSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc
RUN apt -y update \
    && apt -y install build-essential \
    && apt -y install gcc-12 g++-12 gcc-13 g++-13 pandoc \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 12 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 12 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 13 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 13 \
    && apt -y install clang-17 clang-18 clang-tools-18 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-17 17 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-17 17 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 18 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 18
COPY install-dependencies.sh /tmp/
RUN bash /tmp/install-dependencies.sh
CMD /bin/bash
