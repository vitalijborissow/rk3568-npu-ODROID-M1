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

# Read RKNPU_SRAM_PERCENT from Makefile (default 100)
SRAM_PCT=$(grep '^RKNPU_SRAM_PERCENT' "$SCRIPT_DIR/drivers/rknpu/Makefile" \
    | head -1 | grep -oP '\d+$' || echo 100)

# SRAM total: 0xB000 = 45056 bytes = 11 pages of 4096
SRAM_PAGES=11
NPU_PAGES=$((SRAM_PAGES * SRAM_PCT / 100))
RKVDEC_PAGES=$((SRAM_PAGES - NPU_PAGES))

TMP_DTS=$(mktemp /tmp/rknpu-overlay.XXXXXX.dts)

# Build DTS: take everything outside the SRAM markers, inject computed SRAM split
{
    # Part 1: everything before SRAM_SPLIT_BEGIN (strip SRAM_PHANDLE line if 0%)
    awk '/SRAM_SPLIT_BEGIN/{exit} 1' "$SCRIPT_DIR/overlays/rknpu.dts" | \
        if [ "$SRAM_PCT" -eq 0 ]; then grep -v 'SRAM_PHANDLE'; else cat; fi

    # Part 2: SRAM fragment (skip if 0%)
    if [ "$SRAM_PCT" -gt 0 ]; then
        NPU_OFF=$((RKVDEC_PAGES * 4096))
        NPU_SZ=$((NPU_PAGES * 4096))
        NPU_OFF_HEX=$(printf '0x%x' $NPU_OFF)
        NPU_SZ_HEX=$(printf '0x%x' $NPU_SZ)

        if [ "$SRAM_PCT" -lt 100 ]; then
            # Split: rkvdec bottom, NPU top
            RKVDEC_SZ=$((RKVDEC_PAGES * 4096))
            RKVDEC_SZ_HEX=$(printf '0x%x' $RKVDEC_SZ)
            cat <<EOF
	/* SRAM split: ${SRAM_PCT}% NPU (${NPU_SZ}B), ${RKVDEC_SZ}B rkvdec */
	fragment@6 {
		target-path = "/sram@fdcc0000";
		__overlay__ {
			#address-cells = <1>;
			#size-cells = <1>;

			rkvdec-sram@0 {
				reg = <0x0 ${RKVDEC_SZ_HEX}>;
			};

			npu_sram: npu-sram@${NPU_OFF_HEX} {
				reg = <${NPU_OFF_HEX} ${NPU_SZ_HEX}>;
			};
		};
	};
EOF
        else
            # 100%: all SRAM for NPU
            cat <<EOF
	/* NPU SRAM: all 44KB */
	fragment@6 {
		target-path = "/sram@fdcc0000";
		__overlay__ {
			#address-cells = <1>;
			#size-cells = <1>;

			npu_sram: npu-sram@0 {
				reg = <0x0 0xb000>;
			};
		};
	};
EOF
        fi
    fi

    # Part 3: everything after SRAM_SPLIT_END
    awk 'p; /SRAM_SPLIT_END/{p=1}' "$SCRIPT_DIR/overlays/rknpu.dts"
} > "$TMP_DTS"

case "$SRAM_PCT" in
    0)   echo "  SRAM: OFF (rkvdec keeps all 44KB)" ;;
    100) echo "  SRAM: 100% NPU (44KB), rkvdec -> RAM" ;;
    *)   echo "  SRAM: ${SRAM_PCT}% NPU ($((NPU_PAGES*4))KB), rkvdec ($((RKVDEC_PAGES*4))KB)" ;;
esac

dtc -@ -I dts -O dtb -o /boot/overlay-user/rknpu.dtbo "$TMP_DTS" 2>/dev/null
rm -f "$TMP_DTS"
echo "  rknpu.dtbo installed to /boot/overlay-user/"

echo "[4/6] Configuring module autoload..."
echo "rknpu" > /etc/modules-load.d/rknpu.conf
echo "dma32_heap" > /etc/modules-load.d/dma32-heap.conf
# Load all devfreq governors so performance/powersave/userspace are available
cat > /etc/modules-load.d/devfreq-governors.conf << 'EOF'
governor_performance
governor_powersave
governor_userspace
EOF
echo "  Module autoload configured (rknpu + dma32_heap + devfreq governors)."

echo "[5/6] Installing udev rules..."
rm -f /etc/udev/rules.d/99-dma-heap-cma.rules
cat > /etc/udev/rules.d/99-dma-heap-dma32.rules << 'EOF'
ACTION=="add", SUBSYSTEM=="dma_heap", KERNEL=="dma32", RUN+="/bin/sh -c 'rm -f /dev/dma_heap/system && ln -s /dev/dma_heap/dma32 /dev/dma_heap/system'"
EOF
echo "  Udev rules installed (DMA heap → dma32)."

# Clean up stale devfreq services that may clamp NPU frequency
rm -f /etc/udev/rules.d/99-rknpu-devfreq.rules
rm -f /etc/modprobe.d/rknpu-devfreq.conf
for svc in rknpu-devfreq npu-performance; do
    systemctl disable "$svc.service" 2>/dev/null
    systemctl stop "$svc.service" 2>/dev/null
    rm -f "/etc/systemd/system/$svc.service"
done
systemctl daemon-reload
echo "  Stale devfreq services removed (range managed by driver OPP table)."

echo "[6/6] Removing any stale blacklists..."
rm -f /etc/modprobe.d/blacklist-rknpu.conf
rm -f /etc/modprobe.d/blacklist-npu.conf
echo "  Done."

echo ""
echo "=== Installation complete ==="
echo ""
echo "Next steps:"
echo "  1. Edit /boot/armbianEnv.txt and set:"
echo "     user_overlays=rknpu"
echo "     (No custom fdtfile needed — stock Armbian DTB works)"
echo "  2. Reboot"
echo "  3. Verify: lsmod | grep rknpu && ls /dev/rknpu /dev/dri/renderD129"
