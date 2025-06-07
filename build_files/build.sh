#!/bin/bash
set -ouex pipefail

### Install packages
echo "Installing build dependencies..."
dnf5 install -y kernel-devel kernel-headers gcc make kmod openssl

# Get kernel version and set up build environment
KERNEL_VERSION=$(rpm -q kernel --queryformat '%{VERSION}-%{RELEASE}.%{ARCH}')
echo "Building for kernel version: $KERNEL_VERSION"

# Find kernel source directory
KERNEL_SRC_DIR="/usr/src/kernels/$KERNEL_VERSION"
if [ ! -d "$KERNEL_SRC_DIR" ]; then
    KERNEL_SRC_DIR=$(find /usr/src/kernels -maxdepth 1 -type d -name "*" | grep -v "^/usr/src/kernels$" | head -1)
    if [ -z "$KERNEL_SRC_DIR" ] || [ ! -d "$KERNEL_SRC_DIR" ]; then
        echo "ERROR: Kernel source directory not found"
        exit 1
    fi
fi
echo "Using kernel source: $KERNEL_SRC_DIR"

# Generate module signing keys if they don't exist
if [ ! -f "/etc/pki/module-signing/module-signing.key" ]; then
    echo "Generating module signing keys..."
    mkdir -p /etc/pki/module-signing/
    cd /etc/pki/module-signing/
    
    openssl genpkey -algorithm RSA -out module-signing.key -pkcs8 -pkeyopt rsa_keygen_bits:2048
    openssl req -new -x509 -key module-signing.key -out module-signing.crt -days 3650 \
        -subj "/CN=Bazzite Omen Module Signer/"
    chmod 600 module-signing.key
    chmod 644 module-signing.crt
fi

# Create build directory
BUILD_DIR="/tmp/hp-wmi-build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Copy custom hp-wmi.c source
if [ ! -f "/ctx/hp-wmi.c" ]; then
    echo "ERROR: hp-wmi.c source file not found at /ctx/hp-wmi.c"
    exit 1
fi
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
if ! make; then
    echo "ERROR: Failed to build hp-wmi module"
    exit 1
fi

# Verify build success
if [ ! -f "hp-wmi.ko" ]; then
    echo "ERROR: hp-wmi.ko not found after build"
    exit 1
fi

# Sign the kernel module
echo "Signing hp-wmi kernel module..."
if [ -f "$KERNEL_SRC_DIR/scripts/sign-file" ]; then
    $KERNEL_SRC_DIR/scripts/sign-file sha256 \
        /etc/pki/module-signing/module-signing.key \
        /etc/pki/module-signing/module-signing.crt \
        hp-wmi.ko
    echo "Module signed successfully"
else
    echo "WARNING: Module signing script not found - module will be unsigned"
fi

echo "Successfully built hp-wmi.ko"

# Create backup and replace existing modules
echo "Installing hp-wmi kernel module..."
MODULE_INSTALLED=false

# Find and replace existing hp-wmi modules
for module_path in $(find /lib/modules -name "hp-wmi.ko*" 2>/dev/null); do
    echo "Backing up existing module: $module_path"
    cp "$module_path" "$module_path.backup"
    echo "Replacing module: $module_path"
    cp hp-wmi.ko "$module_path"
    MODULE_INSTALLED=true
done

# If no existing modules found, install to extra directory
if [ "$MODULE_INSTALLED" = false ]; then
    EXTRA_DIR="/lib/modules/$KERNEL_VERSION/extra"
    mkdir -p "$EXTRA_DIR"
    cp hp-wmi.ko "$EXTRA_DIR/"
    echo "Installed hp-wmi.ko to $EXTRA_DIR/"
fi

# Update module dependencies
echo "Updating module dependencies..."
depmod -a "$KERNEL_VERSION"

# Create module loading configuration
echo "Creating module configuration..."
cat > /etc/modules-load.d/hp-wmi.conf << EOF
# Load HP WMI module at boot
hp-wmi
EOF

# Create modprobe configuration if needed
cat > /etc/modprobe.d/hp-wmi.conf << EOF
# HP WMI module configuration
# Add any module parameters here if needed
# options hp-wmi parameter=value
EOF

# Clean up build directory
cd /
rm -rf "$BUILD_DIR"

echo "hp-wmi module installation completed successfully!"

### Install additional packages
echo "Installing additional packages..."
dnf5 install -y tmux

### Install Visual Studio Code
echo "Installing Visual Studio Code..."
rpm --import https://packages.microsoft.com/keys/microsoft.asc

cat > /etc/yum.repos.d/vscode.repo << 'EOF'
[code]
name=Visual Studio Code
baseurl=https://packages.microsoft.com/yumrepos/vscode
enabled=1
autorefresh=1
type=rpm-md
gpgcheck=1
gpgkey=https://packages.microsoft.com/keys/microsoft.asc
EOF

dnf5 install -y code
echo "Visual Studio Code installed successfully!"

# Enable services
systemctl enable podman.socket

echo "Build completed successfully!"
echo ""
echo "IMPORTANT NOTES:"
echo "==============="
echo "1. If Secure Boot is enabled, you'll need to import the signing certificate:"
echo "   sudo mokutil --import /etc/pki/module-signing/module-signing.crt"
echo "   Then reboot and follow the MOK enrollment process."
echo ""
echo "2. Alternatively, disable Secure Boot in BIOS/UEFI settings."
echo ""
echo "3. After installation, verify the module loads correctly:"
echo "   sudo modprobe hp-wmi"
echo "   lsmod | grep hp_wmi"
