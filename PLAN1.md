# PLAN1 — Next Steps

> Synthesized from README, FEATURES, OPTIMIZATION, and TODO docs.
> All the core functionality works. What remains is cleanup, hardening, and selective performance work before a clean 1.0 release.

---

## Quick Reference

| Category | Items open |
|----------|-----------|
| Code cleanup (safe, no risk) | 7 items |
| Performance (low risk) | 6 items |
| Performance (medium risk/effort) | 5 items |
| Release validation | 2 items |
| Future / high effort | 4 items |
| Blocked (external dependency) | 1 item |

---

## 1 — Code Cleanup

These are safe, mechanical changes. No behaviour changes, no new bugs possible. Do these first — they reduce noise and make every subsequent change easier to read.

### 1.1 Remove kernel compatibility guards for kernels < 6.1
- **Files:** `rknpu_drv.c`, `rknpu_gem.c`, `rknpu_iommu.c`, `rknpu_job.c`
- **What:** Dozens of `#if KERNEL_VERSION(x,y,0)` blocks covering kernels 4.13, 4.14, 4.15, 4.19, 5.4, 5.5, 5.10. This module targets 6.18+.
- **How:** Delete the old branch in each `#if`/`#else`/`#endif` block, then remove the guard.
- **Effort:** Easy (tedious but mechanical)
- **Value:** High — removes ~200 lines of dead code, makes the driver readable

### 1.2 Remove `FPGA_PLATFORM` guards
- **Files:** `rknpu_drv.c`, `rknpu_reset.c`
- **What:** `#ifndef FPGA_PLATFORM` wraps regulator, devfreq, IOMMU code. This module never runs on FPGA.
- **How:** Delete the `#ifndef FPGA_PLATFORM` / `#endif` wrappers; keep the code inside them.
- **Effort:** Trivial
- **Value:** Medium — removes dead branches, clarifies the power path

### 1.3 Remove `CONFIG_NO_GKI` guards
- **Files:** `rknpu_drv.c`, `rknpu_gem.c`
- **What:** `IS_ENABLED(CONFIG_NO_GKI)` gates the NBUF resource and `cache_sgt_init`. DKMS on Armbian is always non-GKI.
- **How:** Define `CONFIG_NO_GKI` as always-on in the `Makefile`, or remove the guards and keep the non-GKI code path unconditionally.
- **Effort:** Trivial
- **Value:** Medium — potentially enables the NBUF path, simplifies code

### 1.4 Remove duplicate `#include "rknpu_gem.h"` in `rknpu_drv.c`
- **File:** `rknpu_drv.c` lines 49 and 59
- **What:** The header is included once unconditionally and once inside `#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM`.
- **How:** Remove the inner conditional include.
- **Effort:** Trivial (one line)
- **Value:** Low (include guards prevent issues, but it's sloppy)

### 1.5 Remove unused Rockchip vendor headers from `include/rknpu_drv.h`
- **File:** `include/rknpu_drv.h` lines 21–23
- **What:** Includes `rockchip_opp_select.h`, `rockchip_system_monitor.h`, `rockchip_ipa.h` — Rockchip vendor headers that don't exist in Armbian mainline. Currently stubbed by the build system.
- **How:** Check each for actual usage; remove the includes if nothing references them.
- **Effort:** Trivial
- **Value:** Medium — removes build system stub dependency, clarifies public API

### 1.6 Remove dead `//DISABLED:` regulator code in `rknpu_drv.c`
- **File:** `rknpu_drv.c` lines 960–983 and 1124–1136
- **What:** ~30 lines of commented-out `regulator_enable`/`regulator_disable` with `//DISABLED:` prefix in `rknpu_power_on()` and `rknpu_power_off()`. Regulators are managed by the PMIC.
- **How:** Delete the commented-out lines.
- **Effort:** Trivial
- **Value:** Medium — cleans up the power path significantly

### 1.7 Remove `power_get`/`power_put` wrapper from `rknpu_gem_free_object`
- **File:** `rknpu_gem.c` lines 1353–1360
- **What:** `rknpu_gem_free_object()` (the DRM GEM `.free` callback) wraps `rknpu_gem_object_destroy()` with `rknpu_power_get`/`rknpu_power_put_delay`. GEM teardown is a DMA/IOMMU operation — the NPU core does not need to be powered for it.
- **How:** Remove the `rknpu_power_get` / `rknpu_power_put_delay` calls from this function.
- **Effort:** Trivial
- **Value:** Medium — eliminates an unnecessary power cycle per GEM object lifetime

---

## 2 — Performance (Low Risk)

Small, targeted changes with measurable payoff. Each is self-contained.

### 2.1 Remove `clk_get_rate()` calls from the power-on debug block
- **File:** `rknpu_drv.c` lines 993–1002
- **What:** Even at `dev_dbg` level, the `clk_get_rate()` loop executes at runtime (side effects prevent optimisation). It calls `clk_get_rate()` 4× per power-on, each of which acquires a mutex inside the clock framework.
- **How:** Wrap the entire block in `#ifdef DEBUG` / `#endif`, or delete it.
- **Effort:** Trivial
- **Value:** Low-Medium — saves ~4 mutex acquisitions per power-on; adds up in burst workloads

### 2.2 Reduce `msleep(100)` in `rknpu_soft_reset`
- **File:** `rknpu_reset.c` line 114
- **What:** 100 ms sleep during soft reset, to let in-flight jobs drain. The hardware reset assert/deassert needs only ~10 µs (the `udelay(10)` calls at lines 124/129 confirm this).
- **How:** Change `msleep(100)` to `msleep(10)` or `msleep(20)`. Test that reset actually completes cleanly.
- **Effort:** Trivial (but needs a reset-path test)
- **Value:** Medium — faster driver init, faster timeout recovery

### 2.3 Reduce `msleep(100)` in `rknpu_job_abort`
- **File:** `rknpu_job.c` line 617
- **What:** Combined with the `msleep(100)` in soft_reset (which abort calls), a single timeout recovery takes 200+ ms.
- **How:** Reduce to match whatever `rknpu_soft_reset` is reduced to in 2.2.
- **Effort:** Trivial
- **Value:** Medium — faster error recovery for timed-out jobs

### 2.4 Skip `iommu_domain_get_and_switch` for single-domain setups
- **File:** `rknpu_gem.c` lines 859, 994, 1111
- **What:** `rknpu_gem_object_create()`, `rknpu_gem_object_destroy()`, and `rknpu_gem_destroy_ioctl()` each call `rknpu_iommu_domain_get_and_switch()` + `rknpu_iommu_domain_put()`. For the RK3568 (one IOMMU domain, domain_id always 0) these are pure mutex + atomic overhead.
- **How:** Add an early return at the top of `rknpu_iommu_domain_get_and_switch()` when `rknpu_dev->iommu_domain_num == 1`.
- **Effort:** Easy
- **Value:** Medium — saves 2 mutex ops + 2 atomic ops per GEM create/destroy

### 2.5 Remove redundant NULL check in `rknpu_job_subcore_commit_pc`
- **File:** `rknpu_job.c` lines 371–376
- **What:** `task_base = task_kv_addr; if (!task_base) { ... }` — but `task_kv_addr` was already NULL-checked at line 331. Dead branch.
- **How:** Delete the second NULL check.
- **Effort:** Trivial
- **Value:** Low (branch predictor handles it, but dead code is dead code)

### 2.6 Make `power_put_delay` a module parameter
- **File:** `rknpu_drv.c`
- **What:** `power_put_delay` is currently hardcoded to 500 ms. Adding a `module_param` lets users tune without recompiling (0 for power saving, 500–3000 ms for throughput).
- **How:** Add `module_param(power_put_delay, uint, 0644)` with a suitable description.
- **Effort:** Trivial (3 lines)
- **Value:** Low (the 500 ms default is already good; this is user convenience)

---

## 3 — Performance (Medium Risk / Effort)

These require more thought, structural changes, or careful testing. Do after cleanup is done.

### 3.1 `kmem_cache` for `rknpu_job` allocation
- **File:** `rknpu_job.c` line 141
- **What:** Every job submission does `kzalloc(sizeof(*job), GFP_KERNEL)`. The job struct is ~400 bytes; a dedicated slab cache avoids generic allocator overhead.
- **How:** Create a `kmem_cache` in the driver probe path; destroy it in remove. Replace `kzalloc`/`kfree` in `rknpu_job_alloc()`/`rknpu_job_free()`.
- **Effort:** Easy
- **Value:** Medium — ~100–200 ns per submit; measurable at high submission rates

### 3.2 `kmem_cache` for `rknpu_dkms_gem_range` tracking structs
- **File:** `rknpu_gem.c` line 116
- **What:** `rknpu_dkms_track_gem_obj()` does `kzalloc(sizeof(*r))` per GEM object. Same slab-cache argument as 3.1.
- **How:** Same pattern — dedicated cache, created at probe.
- **Effort:** Easy
- **Value:** Low-Medium — fires per buffer allocation, not per inference

### 3.3 Replace linear scan in `rknpu_dkms_find_gem_obj_by_addr` with a hash table
- **File:** `rknpu_gem.c` lines 83–106
- **What:** O(n) `list_for_each_entry` under spinlock. With YOLOv5 using ~15–20 active buffers this is bounded, but hash lookup would be O(1).
- **How:** Replace the `list_head` with a kernel `rhashtable` or a simple `hlist_head[]` array keyed on `dma_addr >> PAGE_SHIFT`.
- **Effort:** Easy-Medium
- **Value:** Medium — saves 0.5–2 µs per submit; more impactful with larger models or many concurrent buffers

### 3.4 Replace linear scan in `rknpu_mem_sync_ioctl`
- **File:** `rknpu_mem.c` lines 553–558
- **What:** Linear scan of `session->list` under spinlock to validate `obj_addr` on every sync call.
- **How:** Same hash table or `xarray` keyed on `obj_addr`.
- **Effort:** Easy-Medium
- **Value:** Low-Medium — sync is not the hottest path

### 3.5 Disable or conditionalize the hrtimer load-tracking loop
- **File:** `rknpu_drv.c` lines 802–841, `include/rknpu_drv.h` line 40
- **What:** The hrtimer fires every 1000 ms, iterates all cores, takes `irq_lock`, and does ktime arithmetic. Its only consumer appears to be the debugfs load display — the devfreq `get_dev_status()` uses `power_refcount`, not this timer.
- **How:** Add a check: only restart the hrtimer if debugfs/procfs load file is open. Or remove entirely if confirmed unused by devfreq.
- **Effort:** Easy
- **Value:** Low — 1 spinlock/second is negligible; mainly about clarity

---

## 4 — Release Validation

These are blocking for a clean 1.0 release regardless of what else gets done.

### 4.1 End-to-end test on a fresh Armbian 6.18.9 install
- **What:** Clone the repo, run `install.sh`, reboot, run the full verification sequence from README.md. Confirm no regressions.
- **Why:** All changes so far have been tested incrementally. A clean-slate test catches missing dependencies, wrong install paths, or udev rule ordering issues.
- **Effort:** Medium (environment setup + systematic testing)

### 4.2 Update README.md to match the current implementation
- **What:** The README still references a `fdtfile` override and an older install flow. It should reflect the current single-overlay, stock-DTB approach and include the verification commands from FEATURES.md.
- **Effort:** Easy

---

## 5 — Future / High Effort

These have real value but require significant research or are not within the scope of a DKMS module alone.

### 5.1 Real hardware counters for devfreq load tracking
- **File:** `rknpu_devfreq_dkms.c` lines 99–125
- **What:** `get_dev_status()` currently returns either 0% or 95% load (binary, based on `power_refcount`). Actual DT/WT bandwidth counters exist in hardware (`rknpu_get_rw_amount()`).
- **Value:** Better power efficiency — the governor could scale down more aggressively during light workloads.
- **Effort:** Medium — integrate rw_amount counters, calibrate thresholds
- **Blocker:** None — purely in-driver change

### 5.2 DMA-BUF buffer pool for the misc device path
- **File:** `rknpu_mem.c` lines 169–200
- **What:** Pre-allocate pools of common buffer sizes to amortize `dma_alloc_coherent()` + `dma_buf_export()` cost across model loads.
- **Value:** Medium — saves 0.5–2 ms per model load; inference latency within a single run is unchanged.
- **Effort:** Medium-High — pool management, size bucketing, LRU eviction

### 5.3 Skip redundant `dma_buf_attach`/`dma_buf_map_attachment` for self-owned buffers
- **File:** `rknpu_mem.c` lines 323–350
- **What:** After allocating a buffer with `dma_alloc_coherent()`, the code immediately attaches and maps it via the DMA-BUF framework — even though the DMA address is already known. This round-trip allocates a scatterlist and calls IOMMU map hooks unnecessarily.
- **Value:** Medium — saves 10–50 µs per buffer allocation
- **Effort:** Medium — need to short-circuit the attach path for owner-allocated buffers while keeping the DMA-BUF FD for userspace mmap

### 5.4 IOMMU translated mode (enable `iommus` on NPU node)
- **What:** Would allow NPU DMA buffers to reside anywhere in physical RAM (above 4 GB), removing the `dma32_heap` constraint entirely.
- **Blocker:** `rk_iommu_is_stall_active()` external abort in `drivers/iommu/rockchip-iommu.c` on Armbian 6.18+. The IOMMU's async `runtime_suspend` callback reads hardware registers after PD6 has already powered off. Fix requires a patch in the kernel IOMMU driver, not in this module.
- **Effort:** Hard (kernel driver change, not DKMS)
- **Workaround in place:** `dma32_heap` + 32-bit DMA mask; works correctly for all current workloads

---

## 6 — Decided / Will Not Do

| Item | Reason |
|------|--------|
| Increase NPU clock above 1000 MHz | SCMI firmware hard cap. 1100 MHz → 594 MHz silently. 1188 MHz crashes the board. |
| PVTPLL clock source for RK3568 | No `clk-pvtpll.c` support, no ring-length tables, no OTP layout. Not feasible without Rockchip silicon data. |
| Zero-copy camera → NPU → display pipeline | Application-level change, requires ISP/GPU driver coordination. Out of scope for this driver module. |
| Replace all 4 clocks with one | Impossible — `aclk`/`hclk`/`pclk` are mandatory hardware bus domains. |

---

## Suggested Order of Work

```
Phase A — Cleanup (no risk, any order):
  1.1 → 1.2 → 1.3 → 1.4 → 1.5 → 1.6 → 1.7

Phase B — Quick performance wins (trivial, high confidence):
  2.1 → 2.3 → 2.2 → 2.5 → 2.4 → 2.6

Phase C — Structural performance (needs testing):
  3.1 → 3.2 → 3.3 → 3.4 → 3.5

Phase D — Release:
  4.2 → 4.1
```

After Phase D the driver is in a clean, releasable state. Items in section 5 are optional improvements that can be done at any time without blocking the release.
