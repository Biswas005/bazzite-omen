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
    # Install kernel headers and build tools \
    rpm-ostree install kernel-devel kernel-headers gcc make && \
    # Get current kernel version \
    KERNEL_VERSION=$(rpm -q kernel --queryformat '%{VERSION}-%{RELEASE}.%{ARCH}') && \
    # Create build directory \
    mkdir -p /tmp/hp-wmi-build && \
    cd /tmp/hp-wmi-build && \
    # Copy custom hp-wmi.c source \
    cp /ctx/hp-wmi.c . && \
    # Create Makefile for building the module \
    cat > Makefile << 'EOF'
obj-m += hp-wmi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
EOF
    # Build the custom module \
    make && \
    # Find and backup original hp-wmi.ko \
    find /lib/modules -name "hp-wmi.ko*" -exec cp {} {}.backup \; && \
    # Replace with custom module \
    find /lib/modules -name "hp-wmi.ko*" -exec cp hp-wmi.ko {} \; && \
    # Update module dependencies \
    depmod -a && \
    # Clean up build artifacts \
    cd / && rm -rf /tmp/hp-wmi-build && \
    # Run the original build script \
    /ctx/build.sh && \
    ostree container commit

### LINTING
## Verify final image and contents are correct.
RUN bootc container lint
