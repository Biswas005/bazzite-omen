# Allow build scripts to be referenced without being copied into the final image
FROM scratch AS ctx
COPY build_files /
COPY hp-wmi.c /

# Base Image
FROM ghcr.io/ublue-os/bazzite:stable
## Other possible base images include:
# FROM ghcr.io/ublue-os/bazzite:latest
# FROM ghcr.io/ublue-os/bluefin-nvidia:stable
# 
# ... and so on, here are more base images
# Universal Blue Images: https://github.com/orgs/ublue-os/packages
# Fedora base image: quay.io/fedora/fedora-bootc:41
# CentOS base images: quay.io/centos-bootc/centos-bootc:stream10

### MODIFICATIONS
## Install kernel development tools and build custom hp-wmi module
RUN --mount=type=bind,from=ctx,source=/,target=/ctx \
    --mount=type=cache,dst=/var/cache \
    --mount=type=cache,dst=/var/log \
    --mount=type=tmpfs,dst=/tmp \
    rpm-ostree install kernel-devel kernel-headers gcc make && \
    mkdir -p /tmp/hp-wmi-build && \
    cd /tmp/hp-wmi-build && \
    cp /ctx/hp-wmi.c . && \
    echo 'obj-m += hp-wmi.o' > Makefile && \
    echo 'KDIR := /lib/modules/$(shell uname -r)/build' >> Makefile && \
    echo 'PWD := $(shell pwd)' >> Makefile && \
    echo '' >> Makefile && \
    echo 'default:' >> Makefile && \
    echo -e '\t$(MAKE) -C $(KDIR) M=$(PWD) modules' >> Makefile && \
    echo '' >> Makefile && \
    echo 'clean:' >> Makefile && \
    echo -e '\t$(MAKE) -C $(KDIR) M=$(PWD) clean' >> Makefile && \
    make && \
    find /lib/modules -name "hp-wmi.ko*" -exec cp {} {}.backup \; && \
    find /lib/modules -name "hp-wmi.ko*" -exec cp hp-wmi.ko {} \; && \
    depmod -a && \
    cd / && rm -rf /tmp/hp-wmi-build && \
    /ctx/build.sh && \
    ostree container commit

### LINTING
## Verify final image and contents are correct.
RUN bootc container lint
