#!/bin/bash
# In your build.sh file

# Install kernel development packages
rpm-ostree install kernel-devel kernel-headers gcc make

# Build and install custom hp-wmi module
KERNEL_VERSION=$(rpm -q kernel --queryformat '%{VERSION}-%{RELEASE}.%{ARCH}')
mkdir -p /tmp/hp-wmi-build
cd /tmp/hp-wmi-build

# Copy and build module
cp /ctx/hp-wmi.c .
cat > Makefile << 'EOF'
obj-m += hp-wmi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
EOF

make
find /lib/modules -name "hp-wmi.ko*" -exec cp {} {}.backup \;
find /lib/modules -name "hp-wmi.ko*" -exec cp hp-wmi.ko {} \;
depmod -a

# Clean up
cd / && rm -rf /tmp/hp-wmi-build

# Your other build commands...
