FROM ubuntu:jammy
RUN apt -y update \
    && apt -y install build-essential \
    && apt -y install gcc-11 g++-11 gcc-10 g++-10 gcc-9 g++-9 pandoc \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 11 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 11 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 10 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 10 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 9 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-9 9 \
    && apt -y install clang-12 clang-11 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-12 12 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-12 12 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-11 11 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-11 11
COPY install-dependencies.sh /tmp/
RUN bash /tmp/install-dependencies.sh
CMD /bin/bash
