#!/bin/bash
set -euo pipefail

echo "Starting Bazzite custom build with hp-wmi module replacement..."

# Install kernel development packages
echo "Installing kernel development packages..."
rpm-ostree install kernel-devel kernel-headers gcc make kmod

# Apply and reboot ostree changes to make kernel headers available
echo "Applying ostree changes..."
rpm-ostree apply-live

# Get the current kernel version
KERNEL_VERSION=$(uname -r)
echo "Current kernel version: $KERNEL_VERSION"

# Check if kernel build directory exists, if not try to find it
KERNEL_BUILD_DIR="/lib/modules/$KERNEL_VERSION/build"
if [ ! -d "$KERNEL_BUILD_DIR" ]; then
    echo "Kernel build directory not found at $KERNEL_BUILD_DIR"
    echo "Searching for kernel build files..."
    
    # Try to find kernel source
    KERNEL_BUILD_DIR="/usr/src/kernels/$KERNEL_VERSION"
    if [ ! -d "$KERNEL_BUILD_DIR" ]; then
        # Try to find any kernel source directory
        KERNEL_BUILD_DIR=$(find /usr/src/kernels -maxdepth 1 -type d -name "*" | grep -v "^/usr/src/kernels$" | head -1)
        if [ -z "$KERNEL_BUILD_DIR" ]; then
            echo "ERROR: Could not find kernel build directory"
            exit 1
        fi
    fi
    
    # Create symlink if it doesn't exist
    echo "Creating symlink from /lib/modules/$KERNEL_VERSION/build to $KERNEL_BUILD_DIR"
    ln -sf "$KERNEL_BUILD_DIR" "/lib/modules/$KERNEL_VERSION/build"
fi

echo "Using kernel build directory: $KERNEL_BUILD_DIR"

# Create build directory
echo "Setting up build environment..."
BUILD_DIR="/tmp/hp-wmi-build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Copy the custom hp-wmi.c source
if [ ! -f "/ctx/hp-wmi.c" ]; then
    echo "ERROR: hp-wmi.c not found in /ctx/"
    exit 1
fi

echo "Copying custom hp-wmi.c source..."
cp /ctx/hp-wmi.c .

# Create Makefile
echo "Creating Makefile..."
cat > Makefile << 'EOF'
obj-m += hp-wmi.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: default clean install
EOF

# Build the module
echo "Building hp-wmi kernel module..."
make

# Check if the module was built successfully
if [ ! -f "hp-wmi.ko" ]; then
    echo "ERROR: hp-wmi.ko was not built successfully"
    exit 1
fi

echo "Successfully built hp-wmi.ko"

# Find and backup the original hp-wmi module
echo "Finding original hp-wmi module(s)..."
ORIGINAL_MODULES=$(find /lib/modules -name "hp-wmi.ko*" 2>/dev/null || true)

if [ -n "$ORIGINAL_MODULES" ]; then
    echo "Found original hp-wmi modules:"
    echo "$ORIGINAL_MODULES"
    
    # Backup original modules
    echo "Backing up original modules..."
    while IFS= read -r module; do
        if [ -f "$module" ]; then
            cp "$module" "$module.backup.$(date +%Y%m%d_%H%M%S)"
            echo "Backed up: $module"
        fi
    done <<< "$ORIGINAL_MODULES"
    
    # Replace with custom module
    echo "Replacing with custom hp-wmi module..."
    while IFS= read -r module; do
        if [ -f "$module" ]; then
            cp hp-wmi.ko "$module"
            echo "Replaced: $module"
        fi
    done <<< "$ORIGINAL_MODULES"
else
    echo "No existing hp-wmi modules found, installing to default location..."
    # Install to the default location for the current kernel
    mkdir -p "/lib/modules/$KERNEL_VERSION/extra"
    cp hp-wmi.ko "/lib/modules/$KERNEL_VERSION/extra/"
    echo "Installed hp-wmi.ko to /lib/modules/$KERNEL_VERSION/extra/"
fi

# Update module dependencies
echo "Updating module dependencies..."
depmod -a

# Verify the module
echo "Verifying module..."
if modinfo hp-wmi >/dev/null 2>&1; then
    echo "hp-wmi module is properly registered"
    modinfo hp-wmi | head -10
else
    echo "WARNING: hp-wmi module may not be properly registered"
fi

# Clean up build directory
echo "Cleaning up build directory..."
cd /
rm -rf "$BUILD_DIR"

echo "hp-wmi module replacement completed successfully!"

# Add any other custom modifications here
echo "Running additional customizations..."

# Example: Install additional packages
# rpm-ostree install some-package

# Example: Configure system settings
# systemctl enable some-service

echo "Build script completed successfully!"
