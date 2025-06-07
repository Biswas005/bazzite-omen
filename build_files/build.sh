#!/bin/bash
set -oue pipefail

echo "Installing essential build dependencies..."

dnf5 install -y kernel-devel kernel-headers gcc make kmod openssl mokutil elfutils-libelf-devel tmux akmod-nvidia xorg-x11-drv-nvidia

# Detect current kernel
KERNEL_VERSION=$(rpm -q kernel --queryformat '%{VERSION}-%{RELEASE}.%{ARCH}')
KERNEL_SRC_DIR="/usr/src/kernels/$KERNEL_VERSION"
echo "Target kernel: $KERNEL_VERSION"

if [ ! -d "$KERNEL_SRC_DIR" ]; then
    echo "ERROR: Kernel source dir not found: $KERNEL_SRC_DIR"
    exit 1
fi

# --- Module Signing Key ---
KEY_DIR="/etc/pki/module-signing"
KEY_PEM="$KEY_DIR/module-signing.key"
CRT_PEM="$KEY_DIR/module-signing.crt"
CRT_DER="$KEY_DIR/module-signing.der"

if [ ! -f "$KEY_PEM" ]; then
    echo "Generating new Secure Boot keys..."
    mkdir -p "$KEY_DIR"
    cd "$KEY_DIR"
    openssl genpkey -algorithm RSA -out module-signing.key -pkeyopt rsa_keygen_bits:2048
    openssl req -new -x509 -key module-signing.key -out module-signing.crt -days 3650 -subj "/CN=Bazzite Omen Module Signer/"
    openssl x509 -in module-signing.crt -outform DER -out module-signing.der
    chmod 600 module-signing.key
    chmod 644 module-signing.crt module-signing.der
    echo "✓ Keys created and saved to $KEY_DIR"
fi

# --- Build and Sign Custom hp-wmi Module ---
BUILD_DIR="/tmp/hp-wmi-build"
mkdir -p "$BUILD_DIR"
cp /ctx/hp-wmi.c "$BUILD_DIR/"
cd "$BUILD_DIR"

cat > Makefile << EOF
obj-m += hp-wmi.o
KDIR := $KERNEL_SRC_DIR
PWD := \$(shell pwd)

default:
	\$(MAKE) -C \$(KDIR) M=\$(PWD) modules

clean:
	\$(MAKE) -C \$(KDIR) M=\$(PWD) clean
EOF

echo "Building hp-wmi..."
make

if [ ! -f "hp-wmi.ko" ]; then
    echo "ERROR: Build failed"
    exit 1
fi

echo "Signing hp-wmi..."
$KERNEL_SRC_DIR/scripts/sign-file sha256 "$KEY_PEM" "$CRT_PEM" hp-wmi.ko

# Install
MODULE_INSTALLED=false
for module_path in $(find /lib/modules -name "hp-wmi.ko*" 2>/dev/null); do
    cp "$module_path" "$module_path.backup"
    cp hp-wmi.ko "$module_path"
    MODULE_INSTALLED=true
done

if [ "$MODULE_INSTALLED" = false ]; then
    EXTRA_DIR="/lib/modules/$KERNEL_VERSION/extra"
    mkdir -p "$EXTRA_DIR"
    cp hp-wmi.ko "$EXTRA_DIR/"
    echo "Installed to: $EXTRA_DIR"
fi

depmod -a "$KERNEL_VERSION"

cat > /etc/modules-load.d/hp-wmi.conf << EOF
hp-wmi
EOF

# --- Sign NVIDIA Modules ---
echo "Signing NVIDIA modules with custom key..."
NVIDIA_MODULE_DIR="/lib/modules/$KERNEL_VERSION/extra/nvidia"

if [ -d "$NVIDIA_MODULE_DIR" ]; then
    for ko in $(find "$NVIDIA_MODULE_DIR" -name "*.ko"); do
        echo "Signing $ko"
        $KERNEL_SRC_DIR/scripts/sign-file sha256 "$KEY_PEM" "$CRT_PEM" "$ko"
    done
    echo "✓ All NVIDIA modules signed"
else
    echo "WARNING: NVIDIA kernel modules not found at $NVIDIA_MODULE_DIR. You may need to reboot first or run 'akmods'."
fi

# --- ujust Recipes ---
echo "Creating ujust Secure Boot recipes..."
mkdir -p /usr/share/ublue-os/just

cat > /usr/share/ublue-os/just/60-hp-wmi-mok.just << 'EOF'
# HP WMI & NVIDIA MOK Handling

enroll-hp-wmi-mok:
    #!/usr/bin/bash
    set -euo pipefail
    mokutil --import /etc/pki/module-signing/module-signing.der

check-hp-wmi-mok:
    #!/usr/bin/bash
    set -euo pipefail
    mokutil --list-enrolled | grep "Bazzite Omen Module Signer" && echo "✓ Enrolled" || echo "✗ Not enrolled"

remove-hp-wmi-mok:
    #!/usr/bin/bash
    set -euo pipefail
    mokutil --delete /etc/pki/module-signing/module-signing.der

test-hp-wmi-module:
    #!/usr/bin/bash
    sudo modprobe -r hp-wmi || true
    sudo modprobe hp-wmi && echo "✓ hp-wmi loaded" || echo "✗ Load failed"

test-nvidia-module:
    #!/usr/bin/bash
    sudo modprobe -r nvidia || true
    sudo modprobe nvidia && echo "✓ nvidia loaded" || echo "✗ Load failed"
EOF

echo "✓ ujust recipes saved"

echo ""
echo "========== FINAL STEPS =========="
echo "1. Enroll signing key:"
echo "   sudo ujust enroll-hp-wmi-mok"
echo "   Reboot and follow MOK Manager to enroll key."
echo ""
echo "2. Confirm:"
echo "   sudo ujust check-hp-wmi-mok"
echo ""
echo "3. Test module loading:"
echo "   sudo ujust test-hp-wmi-module"
echo "   sudo ujust test-nvidia-module"
echo ""
echo "4. Rebuild modules if missing:"
echo "   sudo akmods && sudo depmod -a"
echo ""
echo "================================="

# --- Rust Installation ---
echo "Installing Rust (non-interactively)..."
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
echo "✓ Rust installed"

# --- Brave Browser Installation ---
echo "Installing Brave browser..."
curl -fsS https://dl.brave.com/install.sh | sh
echo "✓ Brave installed"

# --- Remove Firefox ---
echo "Removing Firefox..."
dnf5 remove -y firefox || echo "Firefox not found or already removed"
echo "✓ Firefox removed (if present)"
