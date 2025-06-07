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
    
    # Generate RSA private key
    openssl genpkey -algorithm RSA -out module-signing.key -pkeyopt rsa_keygen_bits:2048
    
    # Generate X.509 certificate
    openssl req -new -x509 -key module-signing.key -out module-signing.crt -days 3650 \
        -subj "/CN=Bazzite Omen Module Signer/"
    
    # Convert certificate to DER format for MOK enrollment
    openssl x509 -in module-signing.crt -outform DER -out module-signing.der
    
    # Set proper permissions
    chmod 600 module-signing.key
    chmod 644 module-signing.crt module-signing.der
    
    echo "Generated signing keys in PEM and DER formats"
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

# Create ujust recipe for MOK enrollment
echo "Creating ujust recipe for MOK enrollment..."
mkdir -p /usr/share/ublue-os/just

cat > /usr/share/ublue-os/just/60-hp-wmi-mok.just << 'EOF'
# HP WMI Module Signing and MOK Management

# Enroll HP WMI module signing certificate in MOK (Machine Owner Key) database
enroll-hp-wmi-mok:
    #!/usr/bin/bash
    set -euo pipefail
    
    MOK_KEY="/etc/pki/module-signing/module-signing.der"
    
    if [ ! -f "$MOK_KEY" ]; then
        echo "ERROR: MOK certificate not found at $MOK_KEY"
        echo "Please ensure the hp-wmi module build script has been run first."
        exit 1
    fi
    
    echo "Enrolling HP WMI module signing certificate in MOK database..."
    echo "You will be prompted to set a password for MOK enrollment."
    echo "Remember this password - you'll need it during the next boot."
    echo ""
    
    if sudo mokutil --import "$MOK_KEY"; then
        echo ""
        echo "SUCCESS: Certificate enrolled in MOK database."
        echo ""
        echo "NEXT STEPS:"
        echo "1. Reboot your system: sudo systemctl reboot"
        echo "2. During boot, you'll see a blue MOK Manager screen"
        echo "3. Select 'Enroll MOK' -> 'Continue' -> 'Yes'"
        echo "4. Enter the password you just set"
        echo "5. Select 'Reboot'"
        echo ""
        echo "After reboot, your custom hp-wmi module will load without issues."
    else
        echo "ERROR: Failed to enroll certificate"
        exit 1
    fi

# Check MOK enrollment status
check-hp-wmi-mok:
    #!/usr/bin/bash
    set -euo pipefail
    
    echo "Checking MOK database for HP WMI certificate..."
    
    if mokutil --list-enrolled | grep -q "Bazzite Omen Module Signer"; then
        echo "✓ HP WMI module signing certificate is enrolled in MOK database"
    else
        echo "✗ HP WMI module signing certificate is NOT enrolled in MOK database"
        echo "Run 'ujust enroll-hp-wmi-mok' to enroll it"
    fi
    
    echo ""
    echo "Secure Boot status:"
    if mokutil --sb-state | grep -q "SecureBoot enabled"; then
        echo "✓ Secure Boot is enabled"
    else
        echo "✗ Secure Boot is disabled"
    fi

# Remove HP WMI certificate from MOK database
remove-hp-wmi-mok:
    #!/usr/bin/bash
    set -euo pipefail
    
    MOK_KEY="/etc/pki/module-signing/module-signing.der"
    
    if [ ! -f "$MOK_KEY" ]; then
        echo "ERROR: MOK certificate not found at $MOK_KEY"
        exit 1
    fi
    
    echo "Removing HP WMI module signing certificate from MOK database..."
    echo "You will be prompted to set a password for MOK removal."
    echo ""
    
    if sudo mokutil --delete "$MOK_KEY"; then
        echo ""
        echo "SUCCESS: Certificate removal request submitted."
        echo "Reboot and follow the MOK Manager prompts to complete removal."
    else
        echo "ERROR: Failed to request certificate removal"
        exit 1
    fi

# Test HP WMI module loading
test-hp-wmi-module:
    #!/usr/bin/bash
    set -euo pipefail
    
    echo "Testing HP WMI module..."
    
    # Remove module if already loaded
    if lsmod | grep -q hp_wmi; then
        echo "Unloading existing hp-wmi module..."
        sudo modprobe -r hp-wmi || true
    fi
    
    # Try to load the module
    echo "Loading hp-wmi module..."
    if sudo modprobe hp-wmi; then
        echo "✓ hp-wmi module loaded successfully"
        
        # Check if module is actually loaded
        if lsmod | grep -q hp_wmi; then
            echo "✓ hp-wmi module is active"
            
            # Show module info
            echo ""
            echo "Module information:"
            modinfo hp-wmi | head -10
        else
            echo "✗ hp-wmi module failed to stay loaded"
        fi
    else
        echo "✗ Failed to load hp-wmi module"
        echo ""
        echo "This might be due to:"
        echo "1. Secure Boot is enabled but certificate is not enrolled in MOK"
        echo "2. Module signature verification failed"
        echo "3. Module compatibility issues"
        echo ""
        echo "Check dmesg for more details: dmesg | tail -20"
    fi

# Show help for HP WMI MOK management
help-hp-wmi-mok:
    @echo "HP WMI Module MOK (Machine Owner Key) Management Commands:"
    @echo ""
    @echo "ujust enroll-hp-wmi-mok    - Enroll signing certificate in MOK database"
    @echo "ujust check-hp-wmi-mok     - Check MOK enrollment status"
    @echo "ujust remove-hp-wmi-mok    - Remove certificate from MOK database"
    @echo "ujust test-hp-wmi-module   - Test loading the hp-wmi module"
    @echo "ujust help-hp-wmi-mok      - Show this help message"
    @echo ""
    @echo "Typical workflow:"
    @echo "1. Build and install the custom hp-wmi module (build script)"
    @echo "2. Enroll the signing certificate: ujust enroll-hp-wmi-mok"
    @echo "3. Reboot and complete MOK enrollment in firmware"
    @echo "4. Test module loading: ujust test-hp-wmi-module"
EOF

echo "ujust recipes created successfully!"

echo "Build completed successfully!"
echo ""
echo "IMPORTANT NOTES:"
echo "==============="
echo "1. Module signing keys have been generated in both PEM and DER formats:"
echo "   - /etc/pki/module-signing/module-signing.crt (PEM)"
echo "   - /etc/pki/module-signing/module-signing.der (DER)"
echo ""
echo "2. If Secure Boot is enabled, enroll the signing certificate using:"
echo "   ujust enroll-hp-wmi-mok"
echo "   Then reboot and follow the MOK enrollment process."
echo ""
echo "3. Check MOK enrollment status with:"
echo "   ujust check-hp-wmi-mok"
echo ""
echo "4. Test module loading with:"
echo "   ujust test-hp-wmi-module"
echo ""
echo "5. For help with MOK management:"
echo "   ujust help-hp-wmi-mok"
echo ""
echo "6. Alternatively, disable Secure Boot in BIOS/UEFI settings."