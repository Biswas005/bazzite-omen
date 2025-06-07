#!/bin/bash
set -ouex pipefail

### Install packages
# Install kernel development tools for building custom hp-wmi module
dnf5 install -y kernel-devel kernel-headers gcc make kmod

# Get kernel version and set up build environment
KERNEL_VERSION=$(rpm -q kernel --queryformat '%{VERSION}-%{RELEASE}.%{ARCH}')
echo "Building for kernel version: $KERNEL_VERSION"

# Find kernel source directory
KERNEL_SRC_DIR="/usr/src/kernels/$KERNEL_VERSION"
if [ ! -d "$KERNEL_SRC_DIR" ]; then
    # Try to find any available kernel source
    KERNEL_SRC_DIR=$(find /usr/src/kernels -maxdepth 1 -type d -name "*" | grep -v "^/usr/src/kernels$" | head -1)
    if [ -z "$KERNEL_SRC_DIR" ] || [ ! -d "$KERNEL_SRC_DIR" ]; then
        echo "ERROR: Kernel source directory not found"
        exit 1
    fi
fi

echo "Using kernel source: $KERNEL_SRC_DIR"

# Create build directory
BUILD_DIR="/tmp/hp-wmi-build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Copy custom hp-wmi.c source
cp /ctx/hp-wmi.c .

# Create Makefile
cat > Makefile << EOF
obj-m += hp-wmi.o
KDIR := $KERNEL_SRC_DIR
PWD := \$(shell pwd)

default:
	\$(MAKE) -C \$(KDIR) M=\$(PWD) modules

clean:
	\$(MAKE) -C \$(KDIR) M=\$(PWD) clean

.PHONY: default clean
EOF

# Build the module
echo "Building hp-wmi kernel module..."
make

# Verify build success
if [ ! -f "hp-wmi.ko" ]; then
    echo "ERROR: Failed to build hp-wmi.ko"
    exit 1
fi

echo "Successfully built hp-wmi.ko"

# Find and replace existing hp-wmi modules
echo "Replacing hp-wmi kernel module..."
find /lib/modules -name "hp-wmi.ko*" -exec cp {} {}.backup \; 2>/dev/null || echo "No existing hp-wmi modules found"
find /lib/modules -name "hp-wmi.ko*" -exec cp hp-wmi.ko {} \; 2>/dev/null || {
    # If no existing modules found, install to extra directory
    mkdir -p "/lib/modules/$KERNEL_VERSION/extra"
    cp hp-wmi.ko "/lib/modules/$KERNEL_VERSION/extra/"
    echo "Installed hp-wmi.ko to /lib/modules/$KERNEL_VERSION/extra/"
}

# Update module dependencies
depmod -a "$KERNEL_VERSION"

# Clean up
cd /
rm -rf "$BUILD_DIR"

echo "hp-wmi module replacement completed successfully!"

### Additional customizations
# Install other packages you need
dnf5 install -y tmux

dnf5 install -y dnf-plugins-core
dnf5 config-manager addrepo --from-repofile=https://brave-browser-rpm-release.s3.brave.com/brave-browser.repo
dnf5 install -y brave-browser


# Enable services
systemctl enable podman.socket

# Use COPR if needed:
# dnf5 -y copr enable your-username/your-repo
# dnf5 -y install your-package
# dnf5 -y copr disable your-username/your-repo

echo "Build completed successfully!"