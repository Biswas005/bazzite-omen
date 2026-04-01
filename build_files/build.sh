#!/usr/bin/bash
set -euo pipefail
set -x

KERNEL_VERSION="$(rpm -q kernel-core --qf '%{VERSION}-%{RELEASE}.%{ARCH}' | head -n1)"
KDIR="/usr/src/kernels/${KERNEL_VERSION}"
BUILD_DIR="/tmp/hp-wmi-build"
SIGN_DIR="/etc/pki/module-signing"

dnf5 install -y   akmods   elfutils-libelf-devel   gcc   git   just   kernel-devel   kernel-headers   kmod   make   mokutil   openssl   toolbox   tmux   vim-enhanced

mkdir -p /usr/lib/bootc/install
cat > /usr/lib/bootc/install/00-omenite.toml <<'EOF'
[install.filesystem.root]
type = "xfs"
EOF

mkdir -p /usr/share/pixmaps /usr/share/icons/hicolor/scalable/apps
install -m 0644 /ctx/assets/omenite-logo.png /usr/share/pixmaps/omenite-logo.png
install -m 0644 /ctx/assets/omenite-logo.svg /usr/share/icons/hicolor/scalable/apps/omenite.svg

if [ -f /usr/lib/os-release ]; then
  sed -i     -e 's/^NAME=.*/NAME="Omenite"/'     -e 's/^PRETTY_NAME=.*/PRETTY_NAME="Omenite Linux"/'     -e 's/^ID=.*/ID=omenite/'     /usr/lib/os-release
  if grep -q '^LOGO=' /usr/lib/os-release; then
    sed -i 's/^LOGO=.*/LOGO=omenite/' /usr/lib/os-release
  else
    echo 'LOGO=omenite' >> /usr/lib/os-release
  fi
fi

mkdir -p /etc/issue.d
cat > /etc/issue.d/10-omenite.issue <<'EOF'
Omenite Linux
Custom Bazzite GNOME-based atomic image for HP Omen systems.
EOF

mkdir -p "${SIGN_DIR}"
if [ -f /ctx/build_files/module-signing.key ]; then
  install -m 0600 /ctx/build_files/module-signing.key "${SIGN_DIR}/module-signing.key"
  install -m 0644 /ctx/build_files/module-signing.crt "${SIGN_DIR}/module-signing.crt"
  install -m 0644 /ctx/build_files/module-signing.der "${SIGN_DIR}/module-signing.der"
else
  openssl genpkey -algorithm RSA -out "${SIGN_DIR}/module-signing.key" -pkeyopt rsa_keygen_bits:2048
  openssl req -new -x509 -key "${SIGN_DIR}/module-signing.key"     -out "${SIGN_DIR}/module-signing.crt" -days 3650     -subj "/CN=Omenite Module Signer/"
  openssl x509 -in "${SIGN_DIR}/module-signing.crt" -outform DER -out "${SIGN_DIR}/module-signing.der"
  chmod 0600 "${SIGN_DIR}/module-signing.key"
  chmod 0644 "${SIGN_DIR}/module-signing.crt" "${SIGN_DIR}/module-signing.der"
fi

test -d "${KDIR}"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cp /ctx/build_files/hp-wmi.c "${BUILD_DIR}/"
cat > "${BUILD_DIR}/Makefile" <<'EOF'
obj-m += hp-wmi.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
EOF

make -C "${KDIR}" M="${BUILD_DIR}" modules

if [ -x "${KDIR}/scripts/sign-file" ]; then
  "${KDIR}/scripts/sign-file" sha256     "${SIGN_DIR}/module-signing.key"     "${SIGN_DIR}/module-signing.crt"     "${BUILD_DIR}/hp-wmi.ko"
fi

mkdir -p "/usr/lib/modules/${KERNEL_VERSION}/extra"
install -m 0644 "${BUILD_DIR}/hp-wmi.ko" "/usr/lib/modules/${KERNEL_VERSION}/extra/hp-wmi.ko"
depmod -a "${KERNEL_VERSION}"

mkdir -p /etc/modules-load.d /etc/modprobe.d
cat > /etc/modules-load.d/hp-wmi.conf <<'EOF'
hp-wmi
EOF

cat > /etc/modprobe.d/omenite-hp-wmi.conf <<'EOF'
# Prefer the custom Omenite hp-wmi module from /usr/lib/modules/*/extra.
EOF

mkdir -p /usr/share/ublue-os/just
cat > /usr/share/ublue-os/just/60-omenite.just <<'EOF'
enroll-omenite-mok:
	#!/usr/bin/bash
	set -euo pipefail
	sudo mokutil --import /etc/pki/module-signing/module-signing.der

check-omenite-mok:
	#!/usr/bin/bash
	set -euo pipefail
	mokutil --list-enrolled | grep -i 'Omenite Module Signer' || true

test-omenite-hp-wmi:
	#!/usr/bin/bash
	set -euo pipefail
	sudo modprobe -r hp-wmi || true
	sudo modprobe hp-wmi
	modinfo hp-wmi | sed -n '1,20p'
EOF

systemctl enable podman.socket
