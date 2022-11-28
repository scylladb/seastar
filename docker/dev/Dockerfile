FROM ubuntu:kinetic
RUN apt -y update \
    && apt -y install build-essential \
    && apt -y install gcc-12 g++-12 gcc-11 g++-11 pandoc \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 12 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 12 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 11 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 11 \
    && apt -y install clang-15 clang-14 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-15 15 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-15 15 \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-14 14 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-14 14
COPY install-dependencies.sh /tmp/
RUN bash /tmp/install-dependencies.sh
CMD /bin/bash
