# RKNPU Feature Status — ODROID-M1 (RK3568)

**Board:** ODROID-M1
**SoC:** RK3568
**DKMS packages:** `rknpu 1.0`, `dma32-heap 1.0`
**Runtime driver string:** `0.9.8`
**Kernel target:** Linux `6.18+`
**Validated baseline:** `6.18.9-current-rockchip64`
**RKNN SDK/runtime tested:** `2.4.0`

---

## Driver Features

| Feature | Build Flag | Runtime Status | Notes |
|---------|------------|----------------|-------|
| Core DKMS driver | `-DRKNPU_DKMS` | ✅ Working | Main kernel module |
| DRM render node | `-DCONFIG_ROCKCHIP_RKNPU_DRM_GEM` | ✅ Working | Validated with `tests/test_drm_gem.c`; stable path is `/dev/dri/by-path/platform-fde40000.npu-render` |
| Misc device `/dev/rknpu` | `-DRKNPU_DKMS_MISCDEV_ENABLED -DRKNPU_DKMS_MISCDEV` | ✅ Working | Direct allocation and DMA-BUF import |
| Fence sync | `-DCONFIG_ROCKCHIP_RKNPU_FENCE` | ✅ Working | DRM syncobj and sync_file support |
| Procfs | `-DCONFIG_ROCKCHIP_RKNPU_PROC_FS` | ✅ Working | 8 runtime entries under `/proc/rknpu/` |
| Debugfs | `-DCONFIG_ROCKCHIP_RKNPU_DEBUG_FS` | ✅ Working | 14 runtime entries under `/sys/kernel/debug/rknpu/` |
| Devfreq / DVFS | `-DCONFIG_PM_DEVFREQ` | ✅ Working | Governors: `simple_ondemand`, `performance`, `powersave`, `userspace` |
| SRAM support | `-DCONFIG_ROCKCHIP_RKNPU_SRAM` | ✅ Working | 44 KB shared with `rkvdec`; controlled by `RKNPU_SRAM_PERCENT` |

---

## Hardware and Device Tree Features

| Feature | Status | Notes |
|---------|--------|-------|
| IOMMU translated mode | ✅ Working | rk3568-iommu v2 |
| NPU power domain PD6 | ✅ Working | Enabled by installed DTB and overlay |
| `vdd_npu` regulator | ✅ Working | RK809 DCDC_REG4 |
| `regulator-always-on` | ✅ Required | Prevents PD6 power-off crash |
| CRU bus clocks | ✅ Working | Required `aclk`, `hclk`, `pclk` domains present |
| SCMI NPU clock | ✅ Working | Sole runtime frequency path |
| OPP table | ✅ Working | `200–1000 MHz` |
| Hardware resets | ✅ Working | `srst_a`, `srst_h` via reset controller |
| NPU IRQ | ✅ Working | GICv3 SPI 151 |
| Thermal throttling | ✅ Working | Bound to CPU and GPU thermal zones, trip at `75°C` |
| SRAM split | ✅ Working | `0–100%` NPU share via `RKNPU_SRAM_PERCENT` |

---

## Armbian Support

| Item | Status | Notes |
|------|--------|-------|
| Armbian release baseline | ✅ Current | `v26.2.1` or newer |
| Armbian-side update | ✅ Required | `armbian/build#9403` |
| Repository kernel target | ✅ Current | Linux `6.18+` |
| Validated kernel baseline | ✅ Tested | `6.18.9-current-rockchip64` |
| ODROID-M1 8 GB runtime | ✅ Verified | Full ~7.5 GB visible on the target board |
| `dma32-heap` DKMS module | ✅ Working | Installed by `install.sh` |
| `/dev/dma_heap/system -> dma32` | ✅ Applied | Installed by udev rule |

---

## Device Nodes

| Device | Status | Purpose |
|--------|--------|---------|
| `/dev/rknpu` | ✅ Present | Misc-device inference and memory path |
| `/dev/dri/by-path/platform-fde40000.npu-render` | ✅ Present | Stable render-node path for DRM GEM access |
| `/dev/dma_heap/system` | ✅ Symlink | Compatibility name expected by RKNN userspace |
| `/dev/dma_heap/dma32` | ✅ Present | DMA heap used by installed runtime |

---

## Driver Internals

| Feature | Status | Notes |
|---------|--------|-------|
| `state_init = rk3576_state_init` | ✅ Applied | Required hardware init register writes |
| `MODULE_DEVICE_TABLE(of, ...)` | ✅ Applied | Enables module autoload via OF modalias |
| Runtime PM | ✅ Working | Power get/put around active work |
| Power-off delay | ✅ Working | Default `500 ms` |
| Soft reset on error | ✅ Working | Includes IOMMU detach and reattach |
| `rknpu_job` slab cache | ✅ Working | `kmem_cache` replaces per-job `kzalloc` |
| GEM range tracking slab cache | ✅ Working | `kmem_cache` for fixed-size range objects |
| Misc-session object-address hash | ✅ Working | Speeds object lookup by `obj_addr` |
| Single-SG MEM_SYNC fast path | ✅ Working | Uses `dma_sync_single_range_*` when `sgt->nents == 1` |
| Busy-time devfreq status | ✅ Working | Uses sampled busy time with active fallback when sampled load is zero |
| GEM non-contiguous allocation | ✅ Working | Cacheable shmem pages through the IOMMU (`RKNPU_GEM_ALLOC_FROM_PAGES`); `dkms_force_contig_alloc=N` by default |

---

## Module Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `dkms_force_contig_alloc` | `N` | Force contiguous DMA allocations and ignore `RKNPU_MEM_NON_CONTIGUOUS` (contiguous buffers are mapped uncached for the CPU) |
| `power_put_delay_ms` | `500` | Delay before powering off the NPU after the last job |

---

## Inference Performance

| Model | Frequency | Avg Latency | FPS |
|-------|-----------|-------------|-----|
| YOLOv5s 640×640 (C API) | 600 MHz | `49.1 ms` | `20.4` |
| YOLOv5s 640×640 (C API) | 1000 MHz | `42.4 ms` | `23.6` |
| YOLOv5s 640×640 (C API, devfreq auto) | auto | `42.4 ms` | `23.6` |
| YOLOv5s 640×640 (Python RKNNLite) | 600 MHz | `96.3 ms` | `10.4` |
| YOLOv5s 640×640 (Python RKNNLite, idle-aware pinned benchmark) | 1000 MHz | about `85–86 ms` | about `11.7` |
| YOLO11n | 600 MHz | about `4.1 ms` | about `241` |
| YOLO11n | 1000 MHz | about `3.1 ms` | about `321` |

Python measurements include userspace wrapper overhead. C API measurements reflect the model runtime more directly.

---

## Voltage and Frequency Matrix

| Frequency | Voltage | Reachable |
|-----------|---------|----------|
| 200 MHz | 825 mV | ✅ Yes |
| ~297 MHz | 825 mV | ✅ Yes |
| 400 MHz | 825 mV | ✅ Yes |
| 600 MHz | 825 mV | ✅ Yes |
| 700 MHz | 900 mV | ✅ Yes |
| 800 MHz | 950 mV | ✅ Yes |
| 900 MHz | 1000 mV | ✅ Yes |
| 1000 MHz | 1050 mV | ✅ Yes |

---

## Known Limitations

| Limitation | Impact | Workaround |
|------------|--------|------------|
| RKNN userspace hardcodes `system` heap | Needs compatibility device name | Installed udev symlink to `dma32` |
| `vdd_npu` must stay always-on | Modest idle power cost | None |
| Stock Armbian DTB lacks required NPU state | Unsupported without installed DTB and overlay | Use `install.sh` |
| SCMI low-end frequency reporting is approximate | Cosmetic only | None needed |
| Frequencies above `1000 MHz` are unsafe | `1100 MHz` maps to `594 MHz`; `1188 MHz` can crash the board | Cap at `1000 MHz` |

---

## Boot Configuration

```text
fdtfile=rockchip/rk3568-odroid-m1-npu.dtb
user_overlays=rknpu
```

The installed DTB is compiled from `dtb/rk3568-odroid-m1-npu.dts`, and `install.sh` installs `overlays/rknpu.dts` as `/boot/overlay-user/rknpu.dtbo`.

---

## System Configuration Files Installed by `install.sh`

| File | Purpose |
|------|---------|
| `/etc/modules-load.d/rknpu.conf` | Autoload `rknpu` |
| `/etc/modules-load.d/dma32-heap.conf` | Autoload `dma32_heap` |
| `/etc/modules-load.d/devfreq-governors.conf` | Autoload performance, powersave, and userspace governors |
| `/etc/udev/rules.d/99-dma-heap-dma32.rules` | Create `/dev/dma_heap/system` symlink |
| `/boot/overlay-user/rknpu.dtbo` | Installed NPU overlay |

