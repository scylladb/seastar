FROM ubuntu:mantic
RUN apt -y update \
    && apt -y install build-essential \
    && apt -y install gcc-12 g++-12 gcc-13 g++-13 pandoc \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 12 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 12 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 13 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 13 \
    && apt -y install clang-16 clang-17 clang-tools-17 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-16 16 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-16 16 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-17 17 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-17 17
COPY install-dependencies.sh /tmp/
RUN bash /tmp/install-dependencies.sh
CMD /bin/bash
