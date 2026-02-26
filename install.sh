#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Rockchip RKNPU DKMS Installer ==="
echo ""

# Check root
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Run as root (sudo $0)"
    exit 1
fi

# Check kernel headers
if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
    echo "ERROR: Kernel headers not found. Install with:"
    echo "  apt install linux-headers-$(uname -r)"
    exit 1
fi

# Check dkms
if ! command -v dkms &>/dev/null; then
    echo "ERROR: dkms not found. Install with: apt install dkms"
    exit 1
fi

# Check dtc
if ! command -v dtc &>/dev/null; then
    echo "ERROR: dtc not found. Install with: apt install device-tree-compiler"
    exit 1
fi

echo "[1/6] Installing rknpu DKMS module..."
dkms remove -m rknpu -v 1.0 --all 2>/dev/null || true
rm -rf /usr/src/rknpu-1.0
mkdir -p /usr/src/rknpu-1.0
cp "$SCRIPT_DIR/dkms.conf" /usr/src/rknpu-1.0/
cp -r "$SCRIPT_DIR/drivers" /usr/src/rknpu-1.0/
dkms add -m rknpu -v 1.0
dkms build -m rknpu -v 1.0
dkms install -m rknpu -v 1.0
echo "  rknpu DKMS installed."

echo "[2/6] Installing dma32-heap DKMS module..."
dkms remove -m dma32-heap -v 1.0 --all 2>/dev/null || true
rm -rf /usr/src/dma32-heap-1.0
mkdir -p /usr/src/dma32-heap-1.0
cp "$SCRIPT_DIR/dma32-heap/dkms.conf" /usr/src/dma32-heap-1.0/
cp "$SCRIPT_DIR/dma32-heap/Makefile" /usr/src/dma32-heap-1.0/
cp "$SCRIPT_DIR/dma32-heap/dma32_heap.c" /usr/src/dma32-heap-1.0/
dkms add -m dma32-heap -v 1.0
dkms build -m dma32-heap -v 1.0
dkms install -m dma32-heap -v 1.0
echo "  dma32-heap DKMS installed."

echo "[3/6] Compiling and installing DT overlay..."
mkdir -p /boot/overlay-user
dtc -@ -I dts -O dtb -o /boot/overlay-user/rknpu.dtbo \
    "$SCRIPT_DIR/overlays/rknpu.dts" 2>/dev/null
echo "  rknpu.dtbo installed to /boot/overlay-user/"

echo "[4/6] Configuring module autoload..."
echo "rknpu" > /etc/modules-load.d/rknpu.conf
echo "dma32_heap" > /etc/modules-load.d/dma32-heap.conf
echo "  Module autoload configured."

echo "[5/6] Installing udev rule (DMA heap CMA redirect)..."
cat > /etc/udev/rules.d/99-dma-heap-cma.rules << 'EOF'
ACTION=="add", SUBSYSTEM=="dma_heap", KERNEL=="linux,cma", RUN+="/bin/sh -c 'rm -f /dev/dma_heap/system && ln -s /dev/dma_heap/linux,cma /dev/dma_heap/system'"
EOF
echo "  Udev rule installed."

echo "[6/6] Removing any stale blacklists..."
rm -f /etc/modprobe.d/blacklist-rknpu.conf
rm -f /etc/modprobe.d/blacklist-npu.conf
echo "  Done."

echo ""
echo "=== Installation complete ==="
echo ""
echo "Next steps:"
echo "  1. Edit /boot/armbianEnv.txt and set:"
echo "     fdtfile=rockchip/rk3568-odroid-m1.no-skip-mmu-read.dtb"
echo "     user_overlays=rknpu"
echo "  2. Reboot"
echo "  3. Verify: lsmod | grep rknpu && ls /dev/rknpu /dev/dri/renderD129"
