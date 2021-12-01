FROM docker.io/fedora:32

COPY install-dependencies.sh .

RUN ./install-dependencies.sh && dnf install -y \
    git \
    clang \
    # end of list