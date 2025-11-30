#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
# Clone only if not already present (robust check)
if [ ! -d "${OUTDIR}/linux-stable/.git" ]; then
    echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
    git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION} "${OUTDIR}/linux-stable"
else
    echo "linux-stable already present in ${OUTDIR}; skipping clone"
fi

# -------------------------
# If Kernel Image not present, build kernel
# -------------------------
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    echo "Cleaning kernel tree..."
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper

    echo "Using virt_defconfig for QEMU virt board..."
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    
    #Make the config smaller (use scripts/config to toggle options)
	# scripts/config is in the kernel tree. We'll disable modules and debug info:
	scripts/config --disable CONFIG_DEBUG_INFO
	scripts/config --disable CONFIG_MODULES
	scripts/config --disable CONFIG_SYSTEM_TRUSTED_KEYS
	# disable printk/KALLSYMS to reduce size:
	scripts/config --disable CONFIG_KALLSYMS
	scripts/config --disable CONFIG_BPF
	# disable many subsystems that produce large code:
	scripts/config --disable CONFIG_DRM
	scripts/config --disable CONFIG_SOUND
	scripts/config --disable CONFIG_NETFILTER
	scripts/config --disable CONFIG_IPV6
	scripts/config --disable CONFIG_NET

    #make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image
    #make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
    
    #echo "menuconfig..."
    #make ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE} menuconfig
    # (disable drivers, unused network/storage, debug options, etc.)
    make ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- olddefconfig

    # build only the Image and DTBs, no modules
    echo "Building kernel (Image) ... (this may take a while)"
    make -j4 ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE} Image dtbs
else
    echo "Kernel Image already present:"
    echo " → ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image"
    echo "Skipping kernel rebuild."
fi

echo "Adding the Image in outdir"
cd "$OUTDIR"
cp "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}/Image"

# -------------------------
# Create staging rootfs layout
# -------------------------
echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"

#if [ -d "${OUTDIR}/rootfs" ]; then
#    echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
#    sudo rm -rf "${OUTDIR}/rootfs"
#fi

# Create necessary base directories
mkdir -p rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,var/log}
mkdir -p rootfs/usr/{bin,lib,sbin}

# -------------------------
# Create device nodes
# -------------------------
cd "${OUTDIR}/rootfs"
mkdir -p dev
# remove old nodes if present
sudo rm -f dev/console dev/null || true
# create device nodes (requires root)
sudo mknod -m 622 dev/console c 5 1
sudo mknod -m 666 dev/null c 1 3
echo "Device nodes created"

# -------------------------
# BusyBox build & install
# -------------------------
cd "$OUTDIR"
# 1. CHECK IF BUSYBOX IS ALREADY INSTALLED
if [ -f "${OUTDIR}/rootfs/bin/busybox" ]; then
    echo "BusyBox is already installed in ${OUTDIR}/rootfs. Skipping build and install."
else
    # 2. IF NOT INSTALLED, PROCEED WITH BUILD
    if [ ! -d "${OUTDIR}/busybox" ]; then
        git clone git://busybox.net/busybox.git
        cd busybox
        git checkout ${BUSYBOX_VERSION}
    else
        cd busybox
    fi

    # Configure busybox (default config)
    echo "Configure busybox (default config)"
    make distclean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

    # Disable TC (Traffic Control) to prevent build errors with missing kernel headers
    sed -i 's/^CONFIG_TC=y/CONFIG_TC=n/' .config
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config

    # Build
    echo "Build busybox (default config)"
    make -j$(nproc) ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE}

    # Install into staging rootfs using CONFIG_PREFIX
    echo "Installing BusyBox into ${OUTDIR}/rootfs"
    make CONFIG_PREFIX="${OUTDIR}/rootfs" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install
fi

# Ensure busybox binaries are executable
chmod -R a+x "${OUTDIR}/rootfs/bin" || true

echo "BusyBox setup complete."

# -------------------------
# Library dependencies
# -------------------------
echo "Library dependencies"
# The busybox binary installed at rootfs/bin/busybox (relative path)
BUSTARGET=${OUTDIR}/rootfs/bin/busybox

# Print interpreter and shared libs (for diagnostics)
${CROSS_COMPILE}readelf -a "${BUSTARGET}" | grep "program interpreter" || true
${CROSS_COMPILE}readelf -a "${BUSTARGET}" | grep "Shared library" || true

# Copy runtime libraries from cross-compiler sysroot (if busybox is dynamically linked)
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)

# Make sure lib directories exist
mkdir -p "${OUTDIR}/rootfs/lib" "${OUTDIR}/rootfs/lib64"

# copy program interpreter if reported (e.g. /lib/ld-linux-aarch64.so.1)
interp=$(${CROSS_COMPILE}readelf -a "${BUSTARGET}" | grep "program interpreter" | awk -F': ' '{print $2}' | sed 's/]//' | tr -d ' \t')
if [ -n "$interp" ]; then
    echo "Copying program interpreter ${interp} from sysroot..."
    # create parent dir in rootfs and copy
    mkdir -p "${OUTDIR}/rootfs/$(dirname ${interp})"
    cp -L "${SYSROOT}${interp}" "${OUTDIR}/rootfs${interp}"
fi

# copy shared libs reported by readelf
libs=$(${CROSS_COMPILE}readelf -a "${BUSTARGET}" | grep "Shared library" | awk -F'[][]' '{print $2}')
for lib in $libs; do
    # try lib64 then lib
    if [ -f "${SYSROOT}/lib64/${lib}" ]; then
        cp -L "${SYSROOT}/lib64/${lib}" "${OUTDIR}/rootfs/lib64/"
    elif [ -f "${SYSROOT}/lib/${lib}" ]; then
        cp -L "${SYSROOT}/lib/${lib}" "${OUTDIR}/rootfs/lib/"
    else
        echo "Warning: library ${lib} not found in sysroot (${SYSROOT})"
    fi
done

echo "Libraries copied from sysroot to rootfs (if needed)."

# -------------------------
# Build and copy the writer utility (cross-compile)
# -------------------------
# Source writer.c path - use FINDER_APP_DIR or explicit path if you prefer
WRITER_SRC="${FINDER_APP_DIR}/writer.c"
if [ ! -f "${WRITER_SRC}" ]; then
    echo "ERROR: writer.c not found at ${WRITER_SRC}"
    # You can point to absolute path if needed:
    # WRITER_SRC="/home/paquitolinux/Documents/assignment-3-JavierFo/finder-app/writer.c"
    exit 1
fi

# Create destination and home dir on rootfs
mkdir -p "${OUTDIR}/rootfs/home"
mkdir -p "${OUTDIR}/rootfs/home/conf"

sudo chown "${USER}:${USER}" "${OUTDIR}/rootfs/home"

WRITER_BIN="${OUTDIR}/rootfs/home/writer"

# --- If a writer binary already exists, remove it ---
if [ -f "$WRITER_BIN" ]; then
    echo "🧹 Found existing writer binary — removing it..."
    sudo rm -f "$WRITER_BIN"
    find $OUTDIR/rootfs/home -name '*.o' -delete
fi

echo "Cross-compiling writer for target (static build preferred)..."
# Try static build to avoid copying libs; fallback to dynamic if static fails

echo "Cross-compiling writer for target (FORCING STATIC BUILD)..."
${CROSS_COMPILE}gcc -static -o "${OUTDIR}/rootfs/home/writer" "${WRITER_SRC}"
if [ $? -ne 0 ]; then
    echo "ERROR: Static cross-compile of writer failed. Cannot guarantee library compatibility."
    exit 1
fi

chmod +x "${OUTDIR}/rootfs/home/writer"
echo "writer copied to ${OUTDIR}/rootfs/home/writer"

# -------------------------
# Copy finder scripts and conf files into rootfs /home
# -------------------------
# Finder app location: use FINDER_APP_DIR (should point to finder-app directory)
if [ -d "${FINDER_APP_DIR}" ]; then
    sudo cp -v "${FINDER_APP_DIR}/conf/username.txt" "${OUTDIR}/rootfs/home/conf" || true
    sudo cp -v "${FINDER_APP_DIR}/conf/assignment.txt" "${OUTDIR}/rootfs/home/conf" || true
    sudo cp -v "${FINDER_APP_DIR}/finder.sh" "${OUTDIR}/rootfs/home/" || true
    sudo chmod +x "${OUTDIR}/rootfs/home/finder.sh" || true
    sudo cp -v "${FINDER_APP_DIR}/finder-test.sh" "${OUTDIR}/rootfs/home/" || true
    sudo chmod +x "${OUTDIR}/rootfs/home/finder-test.sh" || true
else
    echo "Warning: FINDER_APP_DIR ${FINDER_APP_DIR} not found; copy finder files manually."
fi

# -------------------------
# Copy autorun-qemu.sh into rootfs /home (if present)
# -------------------------
if [ -f "${FINDER_APP_DIR}/autorun-qemu.sh" ]; then
    sudo cp -v "${FINDER_APP_DIR}/autorun-qemu.sh" "${OUTDIR}/rootfs/home/" || true
    sudo chmod +x "${OUTDIR}/rootfs/home/autorun-qemu.sh" || true
fi

# -------------------------
# Set ownership to root:root
# -------------------------
echo "Setting ownership to root:root for rootfs"
cd "${OUTDIR}/rootfs"
sudo chown -R root:root .

# -------------------------
# Create init script for initramfs
# -------------------------
echo "Creating init script..."
cd "${OUTDIR}/rootfs"

# We use 'sudo tee' because the directory is now owned by root
sudo tee init > /dev/null << 'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
echo "Booting minimal initramfs..."
/bin/sh
EOF

# Move init script and set permissions
#sudo mv init "${OUTDIR}/rootfs/init"
sudo chmod +x "${OUTDIR}/rootfs/init"

# -------------------------
# Create initramfs.cpio.gz
# -------------------------
cd "${OUTDIR}/rootfs"
echo "Creating initramfs.cpio.gz in ${OUTDIR}"
find . | cpio -H newc -ov --owner root:root 2>/dev/null | gzip -9 > "${OUTDIR}/initramfs.cpio.gz"
echo "initramfs created at ${OUTDIR}/initramfs.cpio.gz"

echo "Build and rootfs complete. Artifacts in ${OUTDIR}:"
ls -l "${OUTDIR}/Image" "${OUTDIR}/initramfs.cpio.gz" || true

