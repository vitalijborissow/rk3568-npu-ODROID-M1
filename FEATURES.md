# RKNPU Feature Status — ODROID-M1 (RK3568)

**Date:** 2026-02-26
**Kernel:** 6.18.9-current-rockchip64 (Armbian)
**Driver:** RKNPU v0.9.8 (DKMS)
**Board:** ODROID-M1, 8 GB LPDDR4
**NPU:** 0.8 TOPS RKNN @ INT8

---

## Driver Features (Makefile Flags)

| # | Feature | Makefile Flag | Compiled | Runtime Status | Notes |
|---|---------|--------------|----------|----------------|-------|
| 1 | Core DKMS driver | `-DRKNPU_DKMS` | ✅ | ✅ Working | Base driver, always enabled |
| 2 | DRM/GEM buffers | `-DCONFIG_ROCKCHIP_RKNPU_DRM_GEM` | ✅ | ✅ Working | `/dev/dri/renderD129` present |
| 3 | Misc device `/dev/rknpu` | `-DRKNPU_DKMS_MISCDEV_ENABLED -DRKNPU_DKMS_MISCDEV` | ✅ | ✅ Working | Direct alloc + DMA-BUF import (full mode) |
| 4 | Fence sync | `-DCONFIG_ROCKCHIP_RKNPU_FENCE` | ✅ | ✅ Working | DRM syncobj/sync_file support |
| 5 | Procfs `/proc/rknpu/` | `-DCONFIG_ROCKCHIP_RKNPU_PROC_FS` | ✅ | ✅ Working | 8 entries: version, freq, load, power, volt, mm, reset, delayms |
| 6 | Debugfs `/sys/kernel/debug/rknpu/` | `-DCONFIG_ROCKCHIP_RKNPU_DEBUG_FS` | ✅ | ✅ Working | 14 entries incl. clock_source, opp_bypass, freq_hz, voltage_mv |
| 7 | Devfreq (DVFS) | `-DCONFIG_PM_DEVFREQ` | ✅ | ✅ Working | simple_ondemand governor, hybrid CRU (≤600 MHz) + SCMI (700–1000 MHz) |
| 8 | SRAM support | `RKNPU_SRAM_PERCENT=100` | ✅ | ✅ Working | 44 KB shared with rkvdec. 0=all video, 50=split, 100=all NPU (default) |

---

## Hardware / DT Features

| # | Feature | Status | Verified | Notes |
|---|---------|--------|----------|-------|
| 9 | IOMMU (translated mode) | ✅ Working | 2026-02-26 | rk3568-iommu v2, iommu group 0 |
| 10 | Power domain PD6 | ✅ Working | 2026-02-26 | Enabled by `rknpu` DT overlay at boot |
| 11 | vdd_npu regulator | ✅ Working | 2026-02-26 | DCDC_REG4 on RK809 PMIC, 900000 µV, enabled=1 |
| 12 | regulator-always-on | ✅ Working | 2026-02-26 | Prevents PD6 power-off crash (set by overlay) |
| 13 | CRU clocks (≤600 MHz) | ✅ Working | 2026-02-26 | 3 clocks: clk=600MHz, aclk=600MHz, hclk=150MHz |
| 14 | SCMI clock (700–1000 MHz) | ✅ Working | 2026-02-26 | Wired in `rknpu` overlay via `&scmi_clk 0x02`. Verified at 1000 MHz. |
| 15 | OPP table (DVFS frequencies) | ✅ Working | 2026-02-26 | 200–1000 MHz all reachable. CRU ≤600 MHz, SCMI 700–1000 MHz. 1 GHz OPP added by overlay. |
| 16 | Hardware resets | ✅ Working | 2026-02-26 | `srst_a`, `srst_h` via reset_control API |
| 17 | NPU IRQ | ✅ Working | 2026-02-26 | GICv3 SPI 151 (0x97), shared with IOMMU |
| 18 | SRAM hardware | ✅ Working | 2026-02-26 | 44 KB SRAM at 0xFDCC0000–0xFDCCB000. Split between NPU and rkvdec via `RKNPU_SRAM_PERCENT`. |
| 19 | Thermal throttling | ✅ Working | 2026-02-27 | Dual-zone: cpu-thermal + gpu-thermal trip 1 (75°C). 2 TSADC (CPU ch0, GPU ch1), no NPU sensor. sustainable-power=905mW, contribution=1024. All 4 governors work: step_wise, fair_share, bang_bang, user_space. power_allocator not in kernel. |

---

## 8 GB RAM Support (3-Layer Fix)

| # | Layer | Status | Verified | Notes |
|---|-------|--------|----------|-------|
| 20 | Kernel IOMMU `GFP_DMA32` patch | ✅ Applied | 2026-02-12 | Compiled into Armbian 6.18.9 kernel. Constrains IOMMU page tables to <4 GB. |
| 21 | Driver GEM `__GFP_DMA` + `ALLOC_FROM_PAGES=0` | ✅ Applied | 2026-02-26 | Forces `dma_alloc_attrs()` path with 32-bit DMA mask |
| 22 | `dma_set_mask_and_coherent(32-bit)` | ✅ Applied | 2026-02-26 | Set in `rknpu_probe()` from `rk356x_rknpu_config.dma_mask` |
| 23 | Udev CMA symlink `/dev/dma_heap/system → linux,cma` | ✅ Applied | 2026-02-26 | Redirects RKNN library allocations to CMA <4 GB |
| 24 | CMA 3 GB `alloc-ranges <4 GB` | ✅ In DTB | 2026-02-26 | DTB reserved-memory node constrains CMA to physical addresses <4 GB |
| 25 | dma32-heap DKMS module | ✅ Working | 2026-02-26 | `/dev/dma_heap/dma32` — backup heap below 4 GB |
| 26 | Full 7.5 GB RAM visible | ✅ Working | 2026-02-26 | No `mem=3584M` needed |

---

## Device Nodes

| # | Device | Status | Purpose |
|---|--------|--------|---------|
| 27 | `/dev/rknpu` | ✅ Present | Misc device — RKNN API job submission (direct alloc + DMA-BUF import) |
| 28 | `/dev/dri/renderD129` | ✅ Present | DRM render node — GEM buffer allocation and sharing |
| 29 | `/dev/dma_heap/system` | ✅ Symlink → `linux,cma` | RKNN runtime buffer allocation (below 4 GB) |
| 30 | `/dev/dma_heap/dma32` | ✅ Present | Backup DMA heap below 4 GB |
| 31 | `/dev/dma_heap/linux,cma` | ✅ Present | CMA heap (3 GB, alloc-ranges <4 GB) |

---

## Driver Internals

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 32 | `state_init = rk3576_state_init` | ✅ Applied | Critical fix — writes NPU HW init registers (0x10, 0x1004, 0x1024). Without this, all jobs timeout with irq_status=0x0. |
| 33 | `MODULE_DEVICE_TABLE(of, ...)` | ✅ Applied | Enables automatic module loading via udev OF modaliases |
| 34 | Runtime PM (power get/put) | ✅ Working | Auto suspend/resume per ioctl. Configurable delay via procfs/debugfs. |
| 35 | Power-off delay | ✅ Working | Default ~500 ms. Tunable via `/proc/rknpu/delayms` or debugfs. |
| 36 | Soft reset on error | ✅ Working | `bypass_soft_reset=0` (enabled). IOMMU detach/reattach on reset. |
| 37 | Job submission (RKNPU_SUBMIT) | ✅ Working | Real inference via librknnrt + DRM/misc paths |
| 38 | DMA-BUF import | ✅ Working | Cross-driver buffer sharing |
| 39 | IOVA allocation | ✅ Working | `alloc_iova_fast()` for IOMMU mappings |
| 40 | GEM contiguous allocation | ✅ Forced | `dkms_force_contig_alloc=Y` (default). Ignores `RKNPU_MEM_NON_CONTIGUOUS`. |

---

## Module Parameters

| # | Parameter | Default | Description |
|---|-----------|---------|-------------|
| 41 | `bypass_irq_handler` | `0` | Bypass IRQ handler (debug) |
| 42 | `bypass_soft_reset` | `0` | Bypass soft reset on error (debug) |
| 43 | `dkms_force_contig_alloc` | `Y` | Force contiguous DMA allocations |
| 44 | `dkms_force_kernel_mapping` | `N` | Force kernel mapping for GEM (debug) |
| 45 | `dkms_gem_addr_log` | `N` | Log GEM address details on GEM_CREATE |
| 46 | `dkms_gem_addr_log_limit` | `64` | Max GEM_CREATE address logs |
| 47 | `dkms_alloc_use_fake_dev` | `N` | Bypass IOMMU DMA ops (test mode) |

---

## Inference Performance

| # | Model | Frequency | Avg Latency | FPS | Verified |
|---|-------|-----------|-------------|-----|----------|
| 48 | YOLOv5s 640×640 (C API) | 600 MHz (CRU) | 49.1 ms | 20.4 | 2026-02-26 |
| 49 | YOLOv5s 640×640 (C API) | 1000 MHz (SCMI) | 39.8 ms | 25.1 | 2026-02-26 |
| 50 | YOLOv5s 640×640 (Python) | 600 MHz (CRU) | 96.3 ms | 10.4 | 2026-02-26 |
| 51 | YOLO11n | 600 MHz | ~4.1 ms | ~241 | 2026-02-12 |
| 52 | YOLO11n | 1000 MHz (SCMI) | ~3.1 ms | ~321 | 2026-02-12 |

**Note:** C API measures `rknn_run()` only. Python includes ~47 ms rknnlite wrapper overhead.

---

## Voltage/Frequency Matrix

| # | Frequency | Voltage | Clock Source | Reachable in Repo Config |
|---|-----------|---------|-------------|--------------------------|
| 53 | 200 MHz | 825 mV | CRU | ✅ Yes |
| 54 | ~297 MHz | 825 mV | CRU | ✅ Yes |
| 55 | 400 MHz | 825 mV | CRU | ✅ Yes |
| 56 | 600 MHz | 825 mV | CRU | ✅ Yes (default) |
| 57 | 700 MHz | 900 mV | SCMI | ✅ Yes |
| 58 | 800 MHz | 950 mV | SCMI | ✅ Yes |
| 59 | 900 MHz | 1000 mV | SCMI | ✅ Yes |
| 60 | 1000 MHz | 1050 mV | SCMI | ✅ Yes |

---

## Known Limitations

| # | Limitation | Impact | Workaround |
|---|------------|--------|------------|
| 61 | ~~Max 600 MHz without SCMI~~ | **FIXED** — SCMI now in repo overlay | 1000 MHz verified, 25.1 FPS YOLOv5s |
| 62 | CMA 3 GB | ~4.5 GB free for general use (CMA is reusable but may fragment) | None — required for NPU DMA <4 GB |
| 63 | RKNN library hardcodes `system` heap | Requires udev symlink workaround | Udev rule installed by `install.sh` |
| 64 | ~~No SRAM acceleration~~ | **FIXED** — 44 KB SRAM split in overlay | `RKNPU_SRAM_PERCENT`: 0=all video, 33/50/66=split, 100=all NPU |
| 65 | `regulator-always-on` on vdd_npu | Modest extra power draw when NPU idle | None — required to prevent PD6 crash |
| 66 | PD6 disabled in stock Armbian DTB | NPU won't work without overlay | `rknpu` overlay enables PD6 at boot |
| 67 | IOMMU GFP_DMA32 patch in kernel | Not in stock Armbian; custom kernel needed for 8 GB | Use Armbian 6.18.9 build that includes this patch |
| 68 | ~~Thermal zone not attached~~ | **FIXED** — dual-zone binding + governor support | Bound to cpu-thermal + gpu-thermal. All 4 compiled governors verified. |
| 69 | DRM path ~50% slower than misc device | Use `/dev/rknpu` for best performance | Both paths available; misc device supports direct alloc since 2026-02-26 |
| 70 | `simple_ondemand` governor idles at low freq | Poor perf if not explicitly set to `performance` | `echo performance > /sys/class/devfreq/fde40000.npu/governor` |
| 71 | CRU freq accuracy | CRU gives approximate values (99/198/297 vs 100/200/300 MHz) | Cosmetic only |
| 72 | SCMI 1100+ MHz crashes | SCMI gap: 1100 maps to 594 MHz, 1188 MHz crashes board | Capped at 1000 MHz |

---

## Boot Configuration (Repo Default)

```
fdtfile=rockchip/rk3568-odroid-m1-npu.dtb
user_overlays=rknpu
```

No `extraargs`, no `mem=3584M`. Full 7.5 GB RAM visible.

---

## System Config Files (Installed by install.sh)

| File | Purpose |
|------|---------|
| `/etc/modules-load.d/rknpu.conf` | Autoload rknpu module at boot |
| `/etc/modules-load.d/dma32-heap.conf` | Autoload dma32_heap module at boot |
| `/etc/udev/rules.d/99-dma-heap-cma.rules` | Symlink `/dev/dma_heap/system` → `linux,cma` |
| `/boot/overlay-user/rknpu.dtbo` | DT overlay: PD6, vdd_npu, power-domains, rknpu-supply |

---

## Feature History

| Date | Milestone |
|------|-----------|
| 2026-01-30 | First working NPU: DRM, IOMMU, PD6, clocks 600 MHz, `mem=3584M` |
| 2026-01-31 | 8 GB RAM fix: `RKNPU_GEM_ALLOC_FROM_PAGES=0`, CMA 3 GB, no `mem=` limit |
| 2026-02-07 | Kernel 6.18.8: API compat fixes (hrtimer, pfn_t, MODULE_IMPORT_NS) |
| 2026-02-07 | SCMI DVFS: full 200–900 MHz range via ARM SCMI clock |
| 2026-02-09 | Full feature set: /dev/rknpu, devfreq, SCMI 1 GHz, SRAM 12 KB, debugfs |
| 2026-02-12 | `state_init = rk3576_state_init` fix — resolves all job timeouts |
| 2026-02-12 | 3-layer 8 GB fix: kernel IOMMU patch + GEM __GFP_DMA + CMA symlink |
| 2026-02-26 | GitHub repo created with full DKMS source, install.sh, overlay, dma32-heap |
| 2026-02-26 | Direct alloc on /dev/rknpu (removed import-only limitation) |
| 2026-02-26 | SCMI clock + OPP 1 GHz wired into repo overlay — full DVFS 200–1000 MHz |
