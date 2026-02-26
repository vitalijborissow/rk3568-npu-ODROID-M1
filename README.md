# Rockchip RKNPU — DKMS Driver for ODROID-M1

DKMS kernel modules to enable the 0.8 TOPS RKNN NPU on the ODROID-M1 (RK3568) running Armbian mainline kernels.

## What's Included

| Component | Description |
|-----------|-------------|
| `drivers/rknpu/` | RKNPU kernel driver (DKMS) — DRM/GEM, devfreq, IOMMU, fence, debugfs, procfs |
| `dma32-heap/` | DMA32 heap module — allocates DMA buffers below 4 GB for 8 GB boards |
| `overlays/rknpu.dts` | Device tree overlay — enables NPU power domain, regulator, IOMMU wiring |
| `install.sh` | One-shot installer |

## Requirements

- ODROID-M1 (Rockchip RK3568) with Armbian
- Kernel ≥ 6.18 with headers installed
- Packages: `dkms`, `build-essential`, `device-tree-compiler`

```bash
apt install dkms build-essential device-tree-compiler linux-headers-current-rockchip64
```

## Install

```bash
git clone https://github.com/vitalijborissow/rk3568-npu-ODROID-M1.git
cd rk3568-npu-ODROID-M1
sudo ./install.sh
```

Then edit `/boot/armbianEnv.txt`:

```ini
fdtfile=rockchip/rk3568-odroid-m1-npu.dtb
user_overlays=rknpu
```

Reboot.

## Verify

```bash
lsmod | grep rknpu           # rknpu module loaded
ls /dev/rknpu                # misc device
ls /dev/dri/renderD129       # DRM render node
ls -la /dev/dma_heap/system  # symlink -> linux,cma
dmesg | grep RKNPU           # probe ok, no errors
```

## 8 GB RAM Support

On 8 GB boards the NPU can only DMA to addresses below 4 GB. This is handled automatically:

- **Kernel IOMMU:** Requires the `GFP_DMA32` patch in `rockchip-iommu.c` (included in Armbian 6.18.9+)
- **Driver GEM:** Forces `dma_alloc_attrs` path with 32-bit DMA mask
- **Userspace heap:** Udev rule redirects `/dev/dma_heap/system` → `linux,cma` (3 GB CMA below 4 GB)

## Device Tree Overlay

The `rknpu` overlay enables:

- **PD6** (NPU power domain) — `status = "okay"`
- **vdd_npu regulator** — `regulator-always-on`, `regulator-boot-on`
- **Power domain wiring** — NPU and IOMMU linked to PD6
- **NPU supply** — `rknpu-supply = <&vdd_npu>`

Without this overlay the NPU power domain stays off and the driver crashes on MMIO access.

## Features

| Feature | Status |
|---------|--------|
| DRM/GEM buffer allocation | ✅ |
| IOMMU (translated mode) | ✅ |
| Devfreq (DVFS, simple_ondemand) | ✅ |
| Thermal throttling | ✅ |
| `/dev/rknpu` misc device | ✅ |
| Debugfs / Procfs | ✅ |
| Fence sync | ✅ |
| 8 GB RAM (full, no mem= limit) | ✅ |
| SCMI clock (>600 MHz) | ❌ requires additional overlay |
| SRAM acceleration | ❌ not wired in ODROID-M1 DTB |

## Tested With

- **Board:** ODROID-M1 8 GB
- **Kernel:** 6.18.9-current-rockchip64 (Armbian)
- **RKNN SDK:** 2.4.0
- **Driver version:** 0.9.8
- **Inference:** YOLOv5 ~56 ms, YOLO11n working

## License

The RKNPU driver is derived from [Rockchip's kernel sources](https://github.com/rockchip-linux/kernel) and is licensed under GPL-2.0. See individual source files for details.
