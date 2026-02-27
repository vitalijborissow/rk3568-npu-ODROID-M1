# AGENTS

This file provides technical context for contributors and AI coding assistants working on this repository.

---

## What This Is

DKMS kernel modules that enable the RKNN NPU on the ODROID-M1 (Rockchip RK3568) under Armbian mainline kernels. The NPU is not supported by any upstream driver — this project extracts and adapts the driver from Rockchip's downstream kernel tree and packages it as a DKMS module that builds against any 6.18+ kernel.

---

## Agent Guidelines

- Do not search recursively through kernel source trees — they are very large
- Read a source file before modifying it; understand existing patterns first
- The driver targets kernel 6.18+ only — do not add compatibility code for older kernels

---

## Platform

| Property | Value |
|----------|-------|
| Board | ODROID-M1 (Hardkernel) |
| SoC | Rockchip RK3568 |
| CPU | Quad-Core Cortex-A55 @ 1.992 GHz, ARMv8-A |
| RAM | 8 GB LPDDR4, 32-bit bus, 3120 MT/s |
| GPU | Mali-G52 MP2, 4× EE @ 650 MHz |
| OS | Armbian Ubuntu (mainline kernel) |
| Kernel | 6.18.x-current-rockchip64 (Armbian) |
| NPU | 0.8 TOPS RKNN @ INT8, device `fde40000.npu` |
| NPU IRQ | GICv3 SPI 151 (0x97) |
| NPU SRAM | 44 KB at 0xFDCC0000–0xFDCCB000 (shared with rkvdec) |

**Why Armbian?** Hardkernel does not provide current OS images for the M1. Armbian provides a maintained mainline-based kernel. Neither Hardkernel nor Rockchip nor Armbian ship NPU drivers for the M1 on mainline — that gap is what this project fills.

---

## KConfig Options

```
CONFIG_ROCKCHIP_RKNPU
CONFIG_ROCKCHIP_RKNPU_PROC_FS
CONFIG_ROCKCHIP_RKNPU_DEBUG_FS
CONFIG_ROCKCHIP_RKNPU_DMA_HEAP
CONFIG_ROCKCHIP_RKNPU_DRM_GEM
CONFIG_ROCKCHIP_RKNPU_FENCE
CONFIG_ROCKCHIP_RKNPU_SRAM
```

---

## Reference Kernel Sources

The driver was derived from the Rockchip downstream kernel. These repositories are the authoritative references when backporting fixes or understanding hardware behaviour:

| Source | Version | URL |
|--------|---------|-----|
| Rockchip kernel | v6.6.x (vanilla v6.6.89 base) | https://github.com/rockchip-linux/kernel.git |
| Hardkernel kernel | v5.10.x | https://github.com/hardkernel/linux.git |
| Armbian build | current | https://github.com/armbian/build.git |
| Vanilla stable | v6.19.x | https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git |

---

## NPU Clock Architecture

Two independent clock paths exist:

- **CRU path** — `clk_npu_src` (npll/gpll 1200 MHz with integer divider) → max 600 MHz
- **SCMI path** — `clk_scmi_npu` (ARM Trusted Firmware via SCMI protocol) → 198, 297, 396, 594, 600, 700, 800, 900, 1000 MHz

This driver uses SCMI exclusively for all frequencies (200–1000 MHz). The earlier CRU/SCMI hybrid approach has been removed.

**Hard limit: 1000 MHz.** 1100 MHz silently falls back to 594 MHz (firmware gap). 1188 MHz crashes the board.

**PVTPLL:** A hardware clock mux exists (CLKSEL_CON7 bit15 selects `npu_pvtpll_out`) but cannot be used — `clk-pvtpll.c` has no RK3568 support, and no ring-length characterisation data exists for this SoC.

**Why 4 clocks?** The three CRU bus clocks (`aclk`, `hclk`, `pclk`) are mandatory hardware clock domains. SCMI controls only the NPU core frequency. All four are required.

### OPP Table

| Frequency | Voltage |
|-----------|---------|
| 200 MHz | 825 mV |
| ~297 MHz | 825 mV |
| 400 MHz | 825 mV |
| 600 MHz | 825 mV (boot default) |
| 700 MHz | 900 mV |
| 800 MHz | 950 mV |
| 900 MHz | 1000 mV |
| 1000 MHz | 1050 mV |

---

## Driver State

**Version:** RKNPU v0.9.8 DKMS
**Tested with:** RKNN SDK 2.4.0

### Working Features

| Feature | Notes |
|---------|-------|
| DRM/GEM buffers | `/dev/dri/renderD129` |
| Misc device | `/dev/rknpu` — direct alloc + DMA-BUF import |
| IOMMU | rk3568-iommu v2, translated mode |
| Devfreq (DVFS) | Governors: simple_ondemand (default), performance, powersave, userspace |
| Thermal throttling | Dual-zone: cpu-thermal + gpu-thermal, 75°C trip |
| Debugfs | `/sys/kernel/debug/rknpu/` — 14 entries |
| Procfs | `/proc/rknpu/` — 8 entries |
| Fence sync | DRM syncobj / sync_file |
| SRAM | 44 KB, configurable 0–100% split with rkvdec via `RKNPU_SRAM_PERCENT` |
| 8 GB RAM | Full 7.5 GB accessible, no `mem=` kernel argument required |

### 8 GB RAM Support

The RK3568 NPU can only DMA to addresses below 4 GB. This is handled in two layers:

1. [Kernel IOMMU `GFP_DMA32` patch](https://github.com/armbian/build/pull/9403) — constrains IOMMU page table allocations to <4 GB (included in Armbian 6.18.9)
2. Driver `dma_set_mask(32-bit)` + udev symlink `/dev/dma_heap/system → dma32`

### Inference Performance

| Model | Frequency | Latency | FPS |
|-------|-----------|---------|-----|
| YOLOv5s 640×640 (C API) | 600 MHz | 49.1 ms | 20.4 |
| YOLOv5s 640×640 (C API) | 1000 MHz | 43.6 ms | 22.9 |
| YOLOv5s 640×640 (C API, devfreq auto) | auto | 43.8 ms | 22.8 |
| YOLOv5s 640×640 (Python) | 600 MHz | 96.3 ms | 10.4 |
| YOLO11n | 600 MHz | ~4.1 ms | ~241 |
| YOLO11n | 1000 MHz | ~3.1 ms | ~321 |

C API measures `rknn_run()` only. Python includes ~47 ms rknnlite overhead. The DRM path (`/dev/dri/renderD129`) and misc path (`/dev/rknpu`) perform identically.

---

## Known Limitations

| Limitation | Notes |
|------------|-------|
| RKNN library hardcodes `system` heap | Worked around by udev symlink `system → dma32` |
| `regulator-always-on` on vdd_npu | Required — disabling it causes a PD6 power domain crash |
| SCMI low-end frequency accuracy | Reports 198/297/396 MHz instead of 200/300/400 — cosmetic only |
| SCMI above 1000 MHz | 1100 MHz silently maps to 594 MHz; 1188 MHz crashes the board |
| NPU DMA buffers constrained to <4 GB physical | By design — the NPU hardware has a 32-bit DMA bus. Buffers are allocated from `dma32_heap`. The full 8 GB is accessible to the CPU; only NPU-owned DMA buffers are below 4 GB. |
| IOMMU translated mode not enabled | Would allow NPU DMA buffers to reside anywhere in physical RAM, but enabling it triggers a `rk_iommu_is_stall_active` external abort in `rockchip-iommu.c` on Armbian 6.18+. Fix requires a change in the kernel IOMMU driver, not this module. |

---

## Open Work

### Code Cleanup
- Remove kernel compatibility blocks for kernels older than 6.1 (`#if KERNEL_VERSION` guards for 4.x/5.x)
- Remove `FPGA_PLATFORM` guards in `rknpu_drv.c` and `rknpu_reset.c`
- Remove `CONFIG_NO_GKI` guards — DKMS on Armbian is always non-GKI
- Remove duplicate `#include "rknpu_gem.h"` in `rknpu_drv.c`
- Remove unused Rockchip vendor headers from `include/rknpu_drv.h`

### Performance
- Remove unnecessary `power_get`/`power_put` from `rknpu_gem_free_object` — GEM object teardown does not require NPU power
- Use `kmem_cache` for `rknpu_job` allocation instead of `kzalloc` per submit
- Replace the linear list scan in `rknpu_dkms_find_gem_obj_by_addr` with a hash table

### Validation
- End-to-end test on a fresh Armbian 6.18.9 install
