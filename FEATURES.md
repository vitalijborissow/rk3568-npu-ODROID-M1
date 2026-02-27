# RKNPU Feature Status — ODROID-M1 (RK3568)

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
| 7 | Devfreq (DVFS) | `-DCONFIG_PM_DEVFREQ` | ✅ | ✅ Working | 4 governors: simple_ondemand (default), performance, powersave, userspace. SCMI-only clocking (200–1000 MHz). Full OPP range unlocked at boot via systemd oneshot. |
| 8 | SRAM support | `RKNPU_SRAM_PERCENT=100` | ✅ | ✅ Working | 44 KB shared with rkvdec. 0=all video, 50=split, 100=all NPU (default) |

---

## Hardware / DT Features

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 9 | IOMMU (translated mode) | ✅ Working | rk3568-iommu v2, iommu group 0 |
| 10 | Power domain PD6 | ✅ Working | Enabled by `rknpu` DT overlay at boot |
| 11 | vdd_npu regulator | ✅ Working | DCDC_REG4 on RK809 PMIC, 900000 µV, enabled=1 |
| 12 | regulator-always-on | ✅ Working | Prevents PD6 power-off crash (set by overlay) |
| 13 | CRU bus clocks | ✅ Working | 3 clocks: clk=600MHz, aclk=600MHz, hclk=150MHz |
| 14 | SCMI clock (all frequencies) | ✅ Working | Sole clock source for DVFS. Wired in `rknpu` overlay via `&scmi_clk 0x02`. |
| 15 | OPP table (DVFS frequencies) | ✅ Working | 200–1000 MHz all reachable via SCMI. 1 GHz OPP added by overlay. |
| 16 | Hardware resets | ✅ Working | `srst_a`, `srst_h` via reset_control API |
| 17 | NPU IRQ | ✅ Working | GICv3 SPI 151 (0x97), shared with IOMMU |
| 18 | SRAM hardware | ✅ Working | 44 KB SRAM at 0xFDCC0000–0xFDCCB000. Split between NPU and rkvdec via `RKNPU_SRAM_PERCENT`. |
| 19 | Thermal throttling | ✅ Working | Dual-zone: cpu-thermal + gpu-thermal trip 1 (75°C). 2 TSADC (CPU ch0, GPU ch1), no NPU sensor. sustainable-power=905mW, contribution=1024. All 4 governors work: step_wise, fair_share, bang_bang, user_space. power_allocator not in kernel. |

---

## 8 GB RAM Support (2-Layer Fix)

| # | Layer | Status | Notes |
|---|-------|--------|-------|
| 20 | Kernel IOMMU `GFP_DMA32` patch | ✅ Applied | Compiled into Armbian 6.18.9 kernel. Constrains IOMMU page tables to <4 GB. |
| 21 | Driver GEM `__GFP_DMA` + `ALLOC_FROM_PAGES=0` | ✅ Applied | Forces `dma_alloc_attrs()` path with 32-bit DMA mask |
| 22 | `dma_set_mask_and_coherent(32-bit)` | ✅ Applied | Set in `rknpu_probe()` from `rk356x_rknpu_config.dma_mask` |
| 23 | Udev dma32 symlink `/dev/dma_heap/system → dma32` | ✅ Applied | Redirects RKNN library allocations to dma32 heap (<4 GB) |
| 24 | dma32-heap DKMS module | ✅ Working | `/dev/dma_heap/dma32` — all allocations guaranteed below 4 GB |
| 25 | Full 7.5 GB RAM visible | ✅ Working | No `mem=3584M` needed, no CMA needed |

---

## Device Nodes

| # | Device | Status | Purpose |
|---|--------|--------|---------|
| 27 | `/dev/rknpu` | ✅ Present | Misc device — RKNN API job submission (direct alloc + DMA-BUF import) |
| 28 | `/dev/dri/renderD129` | ✅ Present | DRM render node — GEM buffer allocation and sharing |
| 29 | `/dev/dma_heap/system` | ✅ Symlink → `dma32` | RKNN runtime buffer allocation (below 4 GB via dma32_heap) |
| 30 | `/dev/dma_heap/dma32` | ✅ Present | Primary DMA heap — all allocations below 4 GB |

---

## Driver Internals

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 32 | `state_init = rk3576_state_init` | ✅ Applied | Critical fix — writes NPU HW init registers (0x10, 0x1004, 0x1024). Without this, all jobs timeout with irq_status=0x0. |
| 33 | `MODULE_DEVICE_TABLE(of, ...)` | ✅ Applied | Enables automatic module loading via udev OF modaliases |
| 34 | Runtime PM (power get/put) | ✅ Working | Auto suspend/resume per ioctl. Configurable delay via procfs/debugfs. |
| 35 | Power-off delay | ✅ Working | Default ~500 ms. Tunable via `/proc/rknpu/delayms` or debugfs. |
| 36 | Soft reset on error | ✅ Working | Always enabled. IOMMU detach/reattach on reset. |
| 37 | Job submission (RKNPU_SUBMIT) | ✅ Working | Real inference via librknnrt + DRM/misc paths |
| 38 | DMA-BUF import | ✅ Working | Cross-driver buffer sharing |
| 39 | IOVA allocation | ✅ Working | `alloc_iova_fast()` for IOMMU mappings |
| 40 | GEM contiguous allocation | ✅ Forced | `dkms_force_contig_alloc=Y` (default). Ignores `RKNPU_MEM_NON_CONTIGUOUS`. |

---

## Module Parameters

| # | Parameter | Default | Description |
|---|-----------|---------|-------------|
| 41 | `dkms_force_contig_alloc` | `Y` | Force contiguous DMA allocations (ignore `RKNPU_MEM_NON_CONTIGUOUS`) |

---

## Inference Performance

| # | Model | Frequency | Avg Latency | FPS |
|---|-------|-----------|-------------|-----|
| 48 | YOLOv5s 640×640 (C API) | 600 MHz | 49.1 ms | 20.4 |
| 49 | YOLOv5s 640×640 (C API) | 1000 MHz | 39.8 ms | 25.1 |
| 50 | YOLOv5s 640×640 (Python) | 600 MHz | 96.3 ms | 10.4 |
| 51 | YOLO11n | 600 MHz | ~4.1 ms | ~241 |
| 52 | YOLO11n | 1000 MHz | ~3.1 ms | ~321 |

**Note:** C API measures `rknn_run()` only. Python includes ~47 ms rknnlite wrapper overhead.

---

## Voltage/Frequency Matrix

| # | Frequency | Voltage | Reachable |
|---|-----------|---------|----------|
| 53 | 200 MHz | 825 mV | ✅ Yes |
| 54 | ~297 MHz | 825 mV | ✅ Yes |
| 55 | 400 MHz | 825 mV | ✅ Yes |
| 56 | 600 MHz | 825 mV | ✅ Yes (boot default) |
| 57 | 700 MHz | 900 mV | ✅ Yes |
| 58 | 800 MHz | 950 mV | ✅ Yes |
| 59 | 900 MHz | 1000 mV | ✅ Yes |
| 60 | 1000 MHz | 1050 mV | ✅ Yes |

---

## Known Limitations

| # | Limitation | Impact | Workaround |
|---|------------|--------|------------|
| 61 | RKNN library hardcodes `system` heap | Requires udev symlink workaround | Udev rule installed by `install.sh` |
| 62 | `regulator-always-on` on vdd_npu | Modest extra power draw when NPU idle | None — required to prevent PD6 crash |
| 63 | PD6 disabled in stock Armbian DTB | NPU won't work without overlay | `rknpu` overlay enables PD6 at boot |
| 64 | IOMMU GFP_DMA32 patch in kernel | Not in stock Armbian; custom kernel needed for 8 GB | Use Armbian 6.18.9 build that includes this patch |
| 65 | DRM path ~50% slower than misc device | Use `/dev/rknpu` for best performance | Both paths available; misc device supports direct alloc |
| 66 | SCMI freq accuracy | SCMI gives approximate values at low end (198/297/396 vs 200/300/400 MHz) | Cosmetic only |
| 67 | SCMI 1100+ MHz crashes | SCMI gap: 1100 maps to 594 MHz, 1188 MHz crashes board | Capped at 1000 MHz |
| 68 | Devfreq boots clamped to 600 MHz | Armbian 6.18 kernel quirk; devfreq core clamps min=max to boot freq | Systemd oneshot writes OPP range to sysfs after boot |

---

## Boot Configuration (Repo Default)

```
user_overlays=rknpu
```

No `fdtfile` override (stock Armbian DTB), no `extraargs`, no `mem=3584M`. Full 7.5 GB RAM visible. No CMA reservation.

---

## System Config Files (Installed by install.sh)

| File | Purpose |
|------|---------|
| `/etc/modules-load.d/rknpu.conf` | Autoload rknpu module at boot |
| `/etc/modules-load.d/dma32-heap.conf` | Autoload dma32_heap module at boot |
| `/etc/udev/rules.d/99-dma-heap-dma32.rules` | Symlink `/dev/dma_heap/system` → `dma32` |
| `/etc/systemd/system/rknpu-devfreq.service` | Unlock devfreq OPP range (200–1000 MHz) at boot |
| `/boot/overlay-user/rknpu.dtbo` | DT overlay: PD6, vdd_npu, power-domains, rknpu-supply |

