#!/bin/bash
set -ouex pipefail

echo "Build script starting..."
echo "Base image: ${BASE_IMAGE:-unknown}"

# Check if we're building on a NVIDIA-enabled base image
NVIDIA_BASE=false
if [[ "${BASE_IMAGE:-}" == *"nvidia"* ]]; then
    NVIDIA_BASE=true
    echo "✓ NVIDIA base image detected - skipping NVIDIA driver installation"
else
    echo "✓ Regular base image detected - will install NVIDIA drivers"
fi

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
cp /ctx/module-signing.crt .
cp /ctx/module-signing.der .

# Check if hp-wmi.c was copied successfully
if [ ! -f "hp-wmi.c" ]; then
    echo "ERROR: Failed to copy hp-wmi.c to build directory"
    exit 1
fi
# Check if module-signing.crt and module-signing.der were copied successfully
if [ ! -f "module-signing.crt" ] || [ ! -f "module-signing.der" ]; then
    echo "ERROR: Failed to copy module-signing.crt or module-signing.der to build directory"
    exit 1
fi

# Persistent Key Management Setup
##################################

# Function to setup persistent keys from GitHub Secrets and local certificates
setup_github_secrets_keys() {
    echo "Setting up persistent keys from GitHub Secrets and local certificates..."

    mkdir -p /etc/pki/module-signing/  

    # Decode the private key from GitHub Secrets environment variable
    if [ -n "${BAZZITE_MODULE_SIGNING_KEY:-}" ]; then  
        echo "$BAZZITE_MODULE_SIGNING_KEY" | base64 -d > /etc/pki/module-signing/module-signing.key  

        # Copy certificate and DER files from /ctx/build_files/ (same directory as hp-wmi.c)
        if [ -f "module-signing.crt" ] && [ -f "module-signing.der" ]; then
            cp module-signing.crt /etc/pki/module-signing/module-signing.crt
            cp module-signing.der /etc/pki/module-signing/module-signing.der

            # Set proper permissions
            chmod 600 /etc/pki/module-signing/module-signing.key  
            chmod 644 /etc/pki/module-signing/module-signing.crt  
            chmod 644 /etc/pki/module-signing/module-signing.der  

            echo "✓ Persistent keys loaded: key from GitHub Secrets, certificates from build directory"
            return 0  
        else
            echo "ERROR: Certificate files (module-signing.crt or module-signing.der) not found in build directory"
            return 1
        fi
    else  
        echo "GitHub Secrets not found - will generate new keys"  
        return 1  
    fi
}

# Install base packages (always needed)
echo "Installing build dependencies..."
dnf5 install -y kernel-devel kernel-headers gcc make kmod openssl mokutil elfutils-libelf-devel tmux

# Conditional NVIDIA Installation
##################################

if [ "$NVIDIA_BASE" = false ]; then
    echo "Installing akmods and NVIDIA drivers..."
    dnf5 install -y akmods
    
    # Try to install NVIDIA packages, but don't fail if they're not available
    if dnf5 install -y akmod-nvidia xorg-x11-drv-nvidia nvidia-settings cuda-devel; then
        echo "✓ NVIDIA packages installed successfully"
        NVIDIA_INSTALLED=true
    else
        echo "⚠️  Some NVIDIA packages failed to install - continuing anyway"
        NVIDIA_INSTALLED=false
    fi
else
    echo "✓ Skipping NVIDIA driver installation (already present in base image)"
    NVIDIA_INSTALLED=false  # Don't try to sign NVIDIA modules later
fi

# Persistent Key Management
############################

echo "Setting up persistent module signing keys..."

if setup_github_secrets_keys; then
    echo "✓ Using persistent keys - users won't need to re-enroll MOK after updates"
    USING_PERSISTENT_KEYS=true
else
    echo "⚠️  Using temporary keys - users will need to re-enroll MOK after each update"
    USING_PERSISTENT_KEYS=false

    # Generate temporary keys if persistent keys not available  
    if [ ! -f "/etc/pki/module-signing/module-signing.key" ]; then  
        echo "Generating temporary module signing keys..."  
        mkdir -p /etc/pki/module-signing/  
        cd /etc/pki/module-signing/  

        BUILD_TIMESTAMP=$(date +%Y%m%d-%H%M%S)  

        # Generate RSA private key  
        openssl genpkey -algorithm RSA -out module-signing.key -pkeyopt rsa_keygen_bits:2048  

        # Generate X.509 certificate with timestamp to indicate temporary nature  
        openssl req -new -x509 -key module-signing.key -out module-signing.crt -days 3650 \
            -subj "/CN=Bazzite Omen Module Signer TEMP-${BUILD_TIMESTAMP}/"  

        # Convert certificate to DER format for MOK enrollment  
        openssl x509 -in module-signing.crt -outform DER -out module-signing.der  

        # Set proper permissions  
        chmod 600 module-signing.key  
        chmod 644 module-signing.crt module-signing.der  

        echo "Generated temporary signing keys in PEM and DER formats"  
    fi
fi

# Show key information for debugging
echo "Certificate Information:"
echo "Subject: $(openssl x509 -in /etc/pki/module-signing/module-signing.crt -noout -subject)"
echo "Fingerprint: $(openssl x509 -in /etc/pki/module-signing/module-signing.crt -fingerprint -noout)"

# Build Custom HP-WMI Module
#############################

# Return to build directory
cd "$BUILD_DIR"

# Create Makefile with proper heredoc syntax
cat > Makefile << 'MAKEFILE_EOF'
obj-m += hp-wmi.o

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: default clean
MAKEFILE_EOF

# Set the KDIR variable for the make command
export KDIR="$KERNEL_SRC_DIR"

# Build the module
echo "Building hp-wmi kernel module..."
echo "Using KDIR: $KDIR"
if ! make KDIR="$KERNEL_SRC_DIR"; then
    echo "ERROR: Failed to build hp-wmi module"
    echo "Makefile contents:"
    cat Makefile
    echo "Current directory: $(pwd)"
    echo "Files in directory:"
    ls -la
    exit 1
fi

# Verify build success
if [ ! -f "hp-wmi.ko" ]; then
    echo "ERROR: hp-wmi.ko not found after build"
    echo "Files in build directory:"
    ls -la
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
cat > /etc/modules-load.d/hp-wmi.conf << 'MODULE_CONF_EOF'
# Load HP WMI module at boot
hp-wmi
MODULE_CONF_EOF

# Create modprobe configuration if needed
cat > /etc/modprobe.d/hp-wmi.conf << 'MODPROBE_CONF_EOF'
# HP WMI module configuration
# Add any module parameters here if needed
options hp-wmi parameter=value
MODPROBE_CONF_EOF

# Clean up build directory
cd /
rm -rf "$BUILD_DIR"

echo "hp-wmi module installation completed successfully!"

# Conditional NVIDIA Module Building and Signing
##################################################

if [ "$NVIDIA_INSTALLED" = true ]; then
    echo "Building and signing NVIDIA modules..."

    # Force akmods to build NVIDIA modules for current kernel
    echo "Running akmods to build NVIDIA modules..."
    akmods --force

    # Wait for akmods to complete and update module dependencies
    depmod -a "$KERNEL_VERSION"

    # Find and sign NVIDIA modules
    echo "Signing NVIDIA modules with persistent keys..."
    NVIDIA_MODULES_FOUND=false

    # Common locations for NVIDIA modules
    NVIDIA_SEARCH_PATHS=(
        "/lib/modules/$KERNEL_VERSION/extra/nvidia"
        "/lib/modules/$KERNEL_VERSION/kernel/drivers/video"
        "/lib/modules/$KERNEL_VERSION/weak-updates/nvidia"
        "/usr/lib/modules/$KERNEL_VERSION/extra/nvidia"
    )

    for search_path in "${NVIDIA_SEARCH_PATHS[@]}"; do
        if [ -d "$search_path" ]; then
            echo "Found NVIDIA modules in: $search_path"
            for ko_file in $(find "$search_path" -name "*.ko" 2>/dev/null); do
                echo "Signing NVIDIA module: $(basename $ko_file)"
                if [ -f "$KERNEL_SRC_DIR/scripts/sign-file" ]; then
                    $KERNEL_SRC_DIR/scripts/sign-file sha256 \
                        /etc/pki/module-signing/module-signing.key \
                        /etc/pki/module-signing/module-signing.crt \
                        "$ko_file"
                    NVIDIA_MODULES_FOUND=true
                fi
            done
        fi
    done

    # Also check for NVIDIA modules in standard kernel locations
    for ko_file in $(find /lib/modules/$KERNEL_VERSION -name "nvidia.ko" 2>/dev/null); do
        echo "Signing NVIDIA module: $(basename $ko_file)"
        if [ -f "$KERNEL_SRC_DIR/scripts/sign-file" ]; then
            $KERNEL_SRC_DIR/scripts/sign-file sha256 \
                /etc/pki/module-signing/module-signing.key \
                /etc/pki/module-signing/module-signing.crt \
                "$ko_file"
            NVIDIA_MODULES_FOUND=true
        fi
    done

    if [ "$NVIDIA_MODULES_FOUND" = true ]; then
        echo "✓ NVIDIA modules signed successfully"
        # Update module dependencies after signing
        depmod -a "$KERNEL_VERSION"
    else
        echo "⚠️  No NVIDIA modules found to sign. They may be built on first boot."
    fi
else
    if [ "$NVIDIA_BASE" = true ]; then
        echo "✓ Skipping NVIDIA module building (using NVIDIA base image)"
    else
        echo "⚠️  Skipping NVIDIA module building (installation failed)"
    fi
fi



# Install Visual Studio Code
##############################

echo "Installing Visual Studio Code..."
rpm --import https://packages.microsoft.com/keys/microsoft.asc

cat > /etc/yum.repos.d/vscode.repo << 'VSCODE_REPO_EOF'
[code]
name=Visual Studio Code
baseurl=https://packages.microsoft.com/yumrepos/vscode
enabled=1
autorefresh=1
type=rpm-md
gpgcheck=1
gpgkey=https://packages.microsoft.com/keys/microsoft.asc
VSCODE_REPO_EOF

dnf5 install -y code
echo "Visual Studio Code installed successfully!"

# Enable services
systemctl enable podman.socket

# Create ujust recipe for MOK enrollment
echo "Creating ujust recipe for MOK enrollment..."
mkdir -p /usr/share/ublue-os/just

cat > /usr/share/ublue-os/just/60-hp-wmi-mok.just << 'UJUST_RECIPE_EOF'
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
UJUST_RECIPE_EOF

echo "ujust recipes created successfully!"

# Final Build Summary
#####################

echo "Build completed successfully!"
echo ""
echo "BUILD SUMMARY:"
echo "=============="
echo "Base Image: ${BASE_IMAGE:-unknown}"
if [ "$NVIDIA_BASE" = true ]; then
    echo "NVIDIA: ✓ Using NVIDIA base image (drivers pre-installed)"
else
    if [ "$NVIDIA_INSTALLED" = true ]; then
        echo "NVIDIA: ✓ Drivers installed via akmods"
    else
        echo "NVIDIA: ⚠️  Driver installation failed or skipped"
    fi
fi
echo ""
echo "IMPORTANT NOTES:"
echo "==============="
echo "1. Module signing keys have been generated/loaded:"
if [ "$USING_PERSISTENT_KEYS" = true ]; then
    echo "   ✓ Using PERSISTENT keys - MOK enrollment survives updates"
else
    echo "   ⚠️  Using TEMPORARY keys - MOK must be re-enrolled after updates"
fi
echo "   - Certificate: /etc/pki/module-signing/module-signing.crt"
echo "   - DER format: /etc/pki/module-signing/module-signing.der"
echo ""
echo "2. If Secure Boot is enabled, enroll the signing certificate:"
echo "   ujust enroll-hp-wmi-mok"
echo ""
echo "3. Check MOK enrollment status:"
echo "   ujust check-hp-wmi-mok"
echo ""
echo "4. Test module loading:"
echo "   ujust test-hp-wmi-module"
echo ""
if [ "$NVIDIA_INSTALLED" = true ]; then
    echo "5. If NVIDIA modules aren't working:"
    echo "   ujust rebuild-nvidia"
    echo ""
fi
echo "6. For complete help:"
echo "   ujust help-hp-wmi-mok"
echo ""
echo "7. Software installed:"
echo "   ✓ HP-WMI custom module (signed)"
if [ "$NVIDIA_BASE" = true ]; then
    echo "   ✓ NVIDIA drivers (pre-installed in base image)"
elif [ "$NVIDIA_INSTALLED" = true ]; then
    echo "   ✓ NVIDIA drivers with akmods (signed)"
else
    echo "   ⚠️  NVIDIA drivers (installation failed)"
fi
echo "   ✓ Rust programming language"
echo "   ✓ Brave browser (Firefox removed)"
echo "   ✓ Visual Studio Code"
if [ "$NVIDIA_INSTALLED" = true ] || [ "$NVIDIA_BASE" = true ]; then
    echo "   ✓ CUDA development tools"
fi
echo ""
if [ "$USING_PERSISTENT_KEYS" = false ]; then
    echo "⚠️  IMPORTANT: Consider setting up persistent key management"
    echo "   for production to avoid MOK re-enrollment after updates!"
fi
echo "================================="