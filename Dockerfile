FROM ubuntu:20.04
LABEL version="1.0"
LABEL description="Seastar Docker Image"

ENV DEBIAN_FRONTEND noninteractive
RUN echo 'debconf debconf/frontend select Noninteractive' | debconf-set-selections

RUN apt update -y && apt install -y git liblzma-dev pkg-config

RUN git clone https://github.com/scylladb/seastar
RUN cd seastar && chmod +x *.sh && apt update -y && ./install-dependencies.sh && ./configure.py --mode=release --prefix=/usr/local && \
        ninja -C build/release install && rm -rf ${IROOT}/seastar

