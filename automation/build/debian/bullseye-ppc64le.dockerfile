FROM debian:bullseye-slim
LABEL maintainer.name="The Xen Project" \
      maintainer.email="xen-devel@lists.xenproject.org"

ENV DEBIAN_FRONTEND=noninteractive
ENV USER root

# Add compiler path
ENV CROSS_COMPILE powerpc64le-linux-gnu-

RUN mkdir /build
WORKDIR /build

# build depends
RUN apt-get update && \
    apt-get --quiet --yes --no-install-recommends install \
        bison \
        build-essential \
        checkpolicy \
        flex \
        gawk \
        gcc-powerpc64le-linux-gnu \
        make \
        python3-minimal \
        # for test phase
        qemu-system-ppc \
        && \
        apt-get autoremove -y && \
        apt-get clean && \
        rm -rf /var/lib/apt/lists* /tmp/* /var/tmp/*
