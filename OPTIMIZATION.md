# RKNPU Driver Optimization Opportunities

> Systematic audit of every source file in this repo.
> Ordered by **priority** (how much it matters), **ease** (how hard to implement), and **impact** (expected speedup or improvement).
>
> **Current baseline:** YOLOv5s 640×640 at 1000 MHz = **43.6 ms** (22.9 FPS)

---

## Tier 1 — High Impact, Easy

### 1. Clean up dead `//DISABLED:` regulator code in `rknpu_drv.c`

- **File:** `rknpu_drv.c:960-983, 1124-1136`
- **What:** ~30 lines of commented-out `regulator_enable`/`regulator_disable` with `//DISABLED:` prefix clutter `rknpu_power_on()` and `rknpu_power_off()`.
- **Why:** Dead code makes the power path harder to read and maintain. If regulators are managed by PMIC, just delete the dead code.
- **Impact:** Code clarity, no runtime change.
- **Ease:** ⭐ Trivial — delete lines.

### 2. Remove `clk_get_rate()` calls from power_on debug block

- **File:** `rknpu_drv.c:993-1002`
- **What:** Even though demoted to `dev_dbg`, the `clk_get_rate()` calls still execute at runtime (the compiler doesn't optimize them away because they have side effects). The loop calls `clk_get_rate()` 4 times per `power_on`.
- **Why:** `clk_get_rate()` takes a mutex internally on some clock frameworks. Removing the entire block (or wrapping in `#ifdef DEBUG`) eliminates 4 mutex acquisitions per power_on.
- **Impact:** ~1-5 µs per power_on. Negligible for single inference, adds up for burst workloads.
- **Ease:** ⭐ Trivial — wrap in `#ifdef DEBUG` or delete.

### 3. Set `power_put_delay` to 500ms (currently 0)

- **File:** `rknpu_drv.c:1567`
- **What:** `rknpu_dev->power_put_delay = 0;` means every `power_put_delay()` call immediately powers off the NPU. With the RKNN library doing ~69 power_get/put cycles per inference, this means the NPU powers on/off repeatedly.
- **Why:** Setting to 500ms keeps NPU powered between rapid ioctl calls, avoiding redundant `clk_bulk_prepare_enable`/`pm_runtime_get_sync` cycles.
- **Impact:** ⭐⭐⭐ **High** — could save 2-5ms per inference by avoiding repeated power cycling. This is likely the single biggest remaining optimization.
- **Ease:** ⭐ Trivial — change `0` to `500`.

### 4. Make `power_put_delay` a module parameter

- **File:** `rknpu_drv.c`
- **What:** Add `module_param` for `power_put_delay` so users can tune it without recompiling.
- **Why:** Different workloads benefit from different delays (0 for power savings, 500-3000 for throughput).
- **Impact:** Flexibility, no runtime change by itself.
- **Ease:** ⭐ Trivial — 3 lines of code.

---

## Tier 2 — Medium Impact, Easy-Medium

### 5. Remove redundant NULL check in `rknpu_job_subcore_commit_pc`

- **File:** `rknpu_job.c:371-376`
- **What:** `task_base = task_kv_addr; if (!task_base) { ... }` — but `task_kv_addr` was already NULL-checked at line 331. This is dead code.
- **Why:** Minor: branch prediction cost on every submit. More importantly, code clarity.
- **Impact:** Negligible runtime, code clarity.
- **Ease:** ⭐ Trivial.

### 6. Use `kmem_cache` (slab) for `rknpu_job` allocation

- **File:** `rknpu_job.c:141`
- **What:** `rknpu_job_alloc()` does `kzalloc(sizeof(*job), GFP_KERNEL)` on every submission. The job struct is ~400 bytes.
- **Why:** `kmem_cache_zalloc()` from a dedicated slab is faster than generic `kzalloc()` for fixed-size, high-frequency allocations — avoids slab lookup overhead.
- **Impact:** ~100-200ns per submit. Measurable at high throughput.
- **Ease:** ⭐⭐ Easy — create cache in probe, destroy in remove, replace kzalloc/kfree.

### 7. Use `kmem_cache` for `rknpu_dkms_gem_range` tracking

- **File:** `rknpu_gem.c:116`
- **What:** `rknpu_dkms_track_gem_obj()` does `kzalloc(sizeof(*r), GFP_KERNEL)` for every GEM object.
- **Why:** Same as above — slab cache for fixed-size struct is faster.
- **Impact:** Minor, but fires on every buffer allocation.
- **Ease:** ⭐⭐ Easy.

### 8. Replace linear list scan in `rknpu_dkms_find_gem_obj_by_addr` with hash table

- **File:** `rknpu_gem.c:83-106`
- **What:** `rknpu_dkms_find_gem_obj_by_addr()` does a linear `list_for_each_entry` scan under spinlock. With many active buffers (YOLOv5 uses ~15-20), this is O(n) per lookup.
- **Why:** The job submission path calls this (indirectly) to resolve task addresses. A hash table (kernel `rhashtable` or simple `hlist_head` array) would make this O(1).
- **Impact:** ⭐⭐ Medium — saves ~0.5-2µs per submit with many buffers.
- **Ease:** ⭐⭐ Easy-Medium — replace list with rhashtable keyed on dma_addr range.

### 9. Replace linear list scan in `rknpu_mem_sync_ioctl`

- **File:** `rknpu_mem.c:553-558`
- **What:** `rknpu_mem_sync_ioctl()` does a linear scan of session->list under spinlock to validate `obj_addr`.
- **Why:** Same O(n) issue. For sessions with many buffers, this adds latency to every sync call.
- **Impact:** Minor — sync is not the hottest path.
- **Ease:** ⭐⭐ Easy — use hash table or xarray keyed on obj_addr.

### 10. Reduce `msleep(100)` in `rknpu_soft_reset`

- **File:** `rknpu_reset.c:114`
- **What:** `msleep(100)` blocks for 100ms during soft reset. This fires on init and on job timeout.
- **Why:** 100ms is very conservative. The hardware reset assert/deassert only needs ~10µs (the `udelay(10)` at lines 124, 129). The 100ms sleep is to let in-flight jobs drain, but the wait queues are woken immediately after. Could be reduced to 10-20ms.
- **Impact:** Faster recovery from timeout, faster driver init.
- **Ease:** ⭐ Trivial — change value.

### 11. Remove `msleep(100)` in `rknpu_job_abort`

- **File:** `rknpu_job.c:617`
- **What:** `msleep(100)` blocks for 100ms during job abort. This fires on every failed job.
- **Why:** Combined with the 100ms in soft_reset (which abort calls), a timeout recovery takes 200ms+. Could be reduced.
- **Impact:** Faster error recovery.
- **Ease:** ⭐ Trivial.

---

## Tier 3 — Medium Impact, Medium Effort

### 12. DMA-BUF buffer pool / cache for misc device path

- **File:** `rknpu_mem.c:169-200`
- **What:** `rknpu_dkms_alloc()` calls `dma_alloc_coherent()` + `dma_buf_export()` for every buffer. The RKNN library allocates and frees buffers per model load.
- **Why:** Pre-allocating a pool of common buffer sizes (4KB, 64KB, 1MB, 4MB) would amortize allocation cost across multiple inference runs.
- **Impact:** ⭐⭐ Medium — saves ~0.5-2ms per model load (not per inference, since buffers are reused within a run).
- **Ease:** ⭐⭐⭐ Medium — need pool management, size bucketing, LRU eviction.

### 13. Eliminate `dma_buf_attach`/`dma_buf_map_attachment` overhead in DKMS alloc

- **File:** `rknpu_mem.c:323-350`
- **What:** After `rknpu_dkms_alloc()`, the code immediately does `dma_buf_attach()` + `dma_buf_map_attachment()` on the same device — but we already have the DMA address from `dma_alloc_coherent()`. This round-trip through the DMA-BUF framework is unnecessary for self-owned buffers.
- **Why:** `dma_buf_attach` + `dma_buf_map_attachment` allocate sg_tables, call IOMMU map hooks, etc. For buffers we just allocated, we already know the dma_addr.
- **Impact:** ⭐⭐ Medium — saves ~10-50µs per buffer allocation.
- **Ease:** ⭐⭐⭐ Medium — need to short-circuit the attach path for owner==1 buffers while keeping the dma_buf FD for userspace mmap.

### 14. Avoid `iommu_domain_get_and_switch` busy-wait loop

- **File:** `rknpu_iommu.c:523-565`
- **What:** `rknpu_iommu_domain_get_and_switch()` has a busy-wait loop with `usleep_range(10, 100)` that can spin for up to 6 seconds if the domain refcount is non-zero.
- **Why:** On single-domain setups (like RK3568 with one IOMMU domain), `domain_id` is always 0 and the early-exit at line 534 fires immediately. But the function still takes a mutex. For the common case, could fast-path with `atomic_read` check before mutex.
- **Impact:** Minor — mutex is ~50ns on uncontended ARM64.
- **Ease:** ⭐⭐ Easy — add lockless fast-path check.

### 15. Remove `iommu_domain_get_and_switch` from GEM create/destroy when domain_id == 0

- **File:** `rknpu_gem.c:859, 994, 1111`
- **What:** `rknpu_gem_object_create()`, `rknpu_gem_object_destroy()`, and `rknpu_gem_destroy_ioctl()` all call `rknpu_iommu_domain_get_and_switch()` + `rknpu_iommu_domain_put()`. For the common case (domain 0, single IOMMU domain), these are pure overhead (mutex lock + atomic inc/dec).
- **Why:** The RK3568 only has one core and one IOMMU domain. The domain never switches.
- **Impact:** ⭐⭐ Medium — saves ~2 mutex operations + 2 atomic ops per GEM create/destroy.
- **Ease:** ⭐⭐ Easy — skip if `rknpu_dev->iommu_domain_num == 1`.

### 16. hrtimer overhead — reduce `RKNPU_LOAD_INTERVAL`

- **File:** `rknpu_drv.c:802-841`, `include/rknpu_drv.h:40`
- **What:** The hrtimer fires every 1000ms (`RKNPU_LOAD_INTERVAL`). It iterates all cores, takes `irq_lock` spinlock, and does ktime arithmetic.
- **Why:** The devfreq polling is 50ms. The hrtimer is used for load tracking but the devfreq `get_dev_status` uses `power_refcount`, not the timer's `busy_time`. The timer's only consumer seems to be the debugger. Could be disabled when debugger is not active.
- **Impact:** Minor — 1 spinlock per second is negligible. But if increased to match devfreq polling, the load tracking would be more accurate.
- **Ease:** ⭐⭐ Easy — make conditional on debugger open, or remove entirely if unused.

---

## Tier 4 — High Impact, High Effort

### 17. Enable IOMMU for full 4GB address space

- **File:** DT overlay, `rknpu_drv.c`, kernel IOMMU driver
- **What:** NPU currently runs in non-IOMMU mode (overlay sets `iommus=<0>`). This means all buffers must be in the DMA32 zone (<4GB physical). With IOMMU, buffers can use all 8GB RAM.
- **Why:** DMA32 zone pressure causes allocation failures for large models. IOMMU also enables scatter-gather (non-contiguous) allocations which are faster when memory is fragmented.
- **Impact:** ⭐⭐⭐ **High** — enables larger models, reduces allocation failures, better memory utilization.
- **Ease:** ⭐⭐⭐⭐ Hard — requires fixing the `rk_iommu_is_stall_active` external abort on Armbian 6.18+ (PD6 power domain issue with Rockchip IOMMU driver).

### 18. Devfreq load tracking with real hardware counters

- **File:** `rknpu_devfreq_dkms.c:99-125`
- **What:** `get_dev_status()` uses `power_refcount` as a binary load indicator (0% or 95%). This gives the governor no granularity.
- **Why:** Real hardware counters (DT/WT read/write amounts from `rknpu_get_rw_amount()`) could provide actual bandwidth utilization, enabling proportional frequency scaling instead of binary max/min.
- **Impact:** ⭐⭐ Medium — better power efficiency, same peak performance.
- **Ease:** ⭐⭐⭐ Medium — need to integrate rw_amount counters into get_dev_status, calibrate thresholds.

### 19. Zero-copy DMA-BUF pipeline (camera → NPU → display)

- **File:** `rknpu_mem.c`, `rknpu_gem.c`
- **What:** Currently, each buffer is allocated, filled by CPU, submitted to NPU, read back by CPU. For camera/video pipelines, DMA-BUF sharing between ISP → NPU → GPU → display would eliminate all CPU copies.
- **Why:** Eliminates 2 memory copies per frame (ISP→RAM→NPU, NPU→RAM→GPU).
- **Impact:** ⭐⭐⭐ **High** for video pipelines — 30-50% total frame time reduction.
- **Ease:** ⭐⭐⭐⭐ Hard — requires application-level changes, ISP/GPU driver coordination.

### 20. SRAM utilization for weight caching

- **File:** `rknpu_gem.c:615-793`, `rknpu_mm.c`
- **What:** The driver has full SRAM allocation support (`rknpu_gem_alloc_buf_with_cache`, `rknpu_mm`), but it's gated behind `CONFIG_NO_GKI` for the NBUF path. The SRAM path works but the RK3568 SRAM (44KB) is shared with rkvdec.
- **Why:** SRAM is ~10x faster than DDR. Caching hot weights or activation tensors in SRAM could significantly reduce memory latency.
- **Impact:** ⭐⭐ Medium — 44KB is too small for full model weights but could cache bias/scale tensors.
- **Ease:** ⭐⭐⭐ Medium — SRAM infra exists, need to wire up allocation hints from RKNN library.

---

## Tier 5 — Code Quality / Maintenance

### 21. Remove version-compat code for kernels < 6.1

- **Files:** `rknpu_drv.c`, `rknpu_gem.c`, `rknpu_iommu.c`, `rknpu_job.c`
- **What:** Dozens of `#if KERNEL_VERSION(x, y, 0) > LINUX_VERSION_CODE` blocks for kernels 4.13, 4.14, 4.15, 4.19, 5.4, 5.5, 5.10. This DKMS module targets 6.18+ only.
- **Why:** Removes ~200 lines of dead code, makes the driver much more readable.
- **Impact:** Code clarity only — no runtime change.
- **Ease:** ⭐⭐ Easy but tedious — many blocks to remove.

### 22. Remove `FPGA_PLATFORM` guards

- **Files:** `rknpu_drv.c`, `rknpu_reset.c`
- **What:** `#ifndef FPGA_PLATFORM` guards wrap regulator, devfreq, IOMMU code. This DKMS module never runs on FPGA.
- **Why:** Dead code removal.
- **Impact:** Code clarity only.
- **Ease:** ⭐ Trivial.

### 23. Remove `CONFIG_NO_GKI` guards

- **Files:** `rknpu_drv.c`, `rknpu_gem.c`
- **What:** `IS_ENABLED(CONFIG_NO_GKI)` gates NBUF resource and cache_sgt_init. DKMS on Armbian is always non-GKI.
- **Why:** Simplify code paths. Define `CONFIG_NO_GKI` in Makefile or remove the guards.
- **Impact:** Code clarity, potentially enables NBUF path.
- **Ease:** ⭐ Trivial.

### 24. Consolidate `rknpu_gem_free_object` power management

- **File:** `rknpu_gem.c:1353-1360`
- **What:** `rknpu_gem_free_object()` wraps `rknpu_gem_object_destroy()` with `rknpu_power_get`/`rknpu_power_put_delay`. But GEM free doesn't need NPU power (it's just DMA/IOMMU operations).
- **Why:** This is the DRM GEM `.free` callback — it fires when the last handle is closed. Adding power_get/put here forces a power cycle that's unnecessary.
- **Impact:** ⭐ Minor — fires once per GEM object lifetime, not per inference.
- **Ease:** ⭐ Trivial — remove the power_get/put wrapper.

### 25. Remove duplicate `#include "rknpu_gem.h"` in `rknpu_drv.c`

- **File:** `rknpu_drv.c:49, 59`
- **What:** `rknpu_gem.h` is included twice — once unconditionally (line 49) and once inside `#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM` (line 59).
- **Why:** Minor: include guards prevent issues but it's sloppy.
- **Impact:** None.
- **Ease:** ⭐ Trivial.

### 26. Remove stale Rockchip header includes from `rknpu_drv.h`

- **File:** `include/rknpu_drv.h:21-23`
- **What:** Includes `rockchip_opp_select.h`, `rockchip_system_monitor.h`, `rockchip_ipa.h` — these are Rockchip vendor headers that don't exist on Armbian mainline.
- **Why:** The Makefile's `-I$(src)/include` path must provide stubs or these are dead includes. Removing them makes the header self-contained.
- **Impact:** Build clarity.
- **Ease:** ⭐ Trivial — check if used, remove if not.

---

## Summary Table

| # | Optimization | Priority | Ease | Expected Impact |
|---|-------------|----------|------|-----------------|
| **3** | **power_put_delay = 500ms** | ⭐⭐⭐ | ⭐ | **2-5ms/inference** |
| **17** | **Enable IOMMU** | ⭐⭐⭐ | ⭐⭐⭐⭐ | Large models, 8GB RAM |
| **19** | **Zero-copy DMA-BUF pipeline** | ⭐⭐⭐ | ⭐⭐⭐⭐ | 30-50% for video |
| **13** | Skip DMA-BUF attach for self-owned bufs | ⭐⭐ | ⭐⭐⭐ | 10-50µs/alloc |
| **12** | Buffer pool for misc device | ⭐⭐ | ⭐⭐⭐ | 0.5-2ms/model load |
| **15** | Skip domain switch for single-domain | ⭐⭐ | ⭐⭐ | 2 mutex ops/GEM op |
| **8** | Hash table for GEM range lookup | ⭐⭐ | ⭐⭐ | 0.5-2µs/submit |
| **6** | kmem_cache for job alloc | ⭐⭐ | ⭐⭐ | 100-200ns/submit |
| **18** | HW counter devfreq tracking | ⭐⭐ | ⭐⭐⭐ | Better power efficiency |
| **20** | SRAM weight caching | ⭐⭐ | ⭐⭐⭐ | Depends on model |
| **4** | Module param for power_put_delay | ⭐ | ⭐ | Flexibility |
| **10** | Reduce msleep in soft_reset | ⭐ | ⭐ | Faster init/recovery |
| **11** | Reduce msleep in job_abort | ⭐ | ⭐ | Faster error recovery |
| **2** | Remove clk_get_rate from power_on | ⭐ | ⭐ | ~5µs/power_on |
| **1** | Clean up //DISABLED: code | ⭐ | ⭐ | Code clarity |
| **24** | Remove power_get from gem_free_object | ⭐ | ⭐ | Minor |
| **21** | Remove old kernel compat code | ⭐ | ⭐⭐ | Code clarity |
| **22** | Remove FPGA_PLATFORM guards | ⭐ | ⭐ | Code clarity |
| **23** | Remove CONFIG_NO_GKI guards | ⭐ | ⭐ | Code clarity |

> **⭐ Ease:** 1=trivial, 2=easy, 3=medium, 4=hard
> **⭐ Priority/Impact:** 1=minor, 2=medium, 3=high

---

## Already Done (This Session)

| Optimization | Result |
|-------------|--------|
| Remove power_get/put from GEM ioctls (DRM+misc) | 170→69 calls/inference |
| Demote LOG_ERROR→LOG_DEBUG in GEM_CREATE, MEM_SYNC | Eliminated printk on every buffer op |
| Demote dev_info→dev_dbg in POWER_ON clock logging | 345→0 dmesg lines/inference |
| Demote dev_info→dev_dbg in job submission | 3 fewer printk/submit |
| **Total improvement** | **46.4→43.6ms (6% faster)** |
