FROM fedora:29

COPY install-dependencies.sh .

RUN ./install-dependencies.sh && dnf install -y \
    git \
    # end of list