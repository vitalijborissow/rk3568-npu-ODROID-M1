/*
 * test_direct_alloc.c — Verify /dev/rknpu direct allocation + import paths
 *
 * Build:  gcc -o test_direct_alloc test_direct_alloc.c
 * Run:    ./test_direct_alloc
 *
 * Tests both MEM_CREATE paths:
 *   handle=0  → direct allocation (dma_alloc_coherent + dma_buf_export)
 *   handle>0  → DMA-BUF import (from /dev/dma_heap/linux,cma)
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define RKNPU_MEM_CREATE 0x02
#define RKNPU_MEM_DESTROY 0x04

struct rknpu_mem_create {
    uint32_t handle;
    uint32_t flags;
    uint64_t size;
    uint64_t obj_addr;
    uint64_t dma_addr;
    uint64_t sram_size;
    int32_t iommu_domain_id;
    uint32_t core_mask;
};

struct rknpu_mem_destroy {
    uint32_t handle;
    uint32_t reserved;
    uint64_t obj_addr;
};

int main() {
    int fd = open("/dev/rknpu", O_RDWR);
    if (fd < 0) { perror("open /dev/rknpu"); return 1; }

    /* Test 1: Direct allocation (handle=0) */
    struct rknpu_mem_create mc = {0};
    mc.handle = 0;  /* direct alloc, NOT import */
    mc.size = 1024 * 1024;  /* 1 MB */
    int ret = ioctl(fd, _IOWR('R', RKNPU_MEM_CREATE, struct rknpu_mem_create), &mc);
    if (ret < 0) {
        printf("DIRECT ALLOC (handle=0): FAILED errno=%d (%s)\n", errno, strerror(errno));
    } else {
        printf("DIRECT ALLOC (handle=0): OK\n");
        printf("  returned handle(fd)=%u\n", mc.handle);
        printf("  size=%llu\n", (unsigned long long)mc.size);
        printf("  dma_addr=0x%llx\n", (unsigned long long)mc.dma_addr);
        printf("  obj_addr=0x%llx\n", (unsigned long long)mc.obj_addr);
        printf("  dma_addr < 4GB: %s\n", mc.dma_addr < 0x100000000ULL ? "YES" : "NO");

        /* Cleanup */
        struct rknpu_mem_destroy md = {0};
        md.handle = mc.handle;
        md.obj_addr = mc.obj_addr;
        ret = ioctl(fd, _IOWR('R', RKNPU_MEM_DESTROY, struct rknpu_mem_destroy), &md);
        printf("  destroy: %s\n", ret == 0 ? "OK" : "FAILED");
    }

    /* Test 2: Import (handle>0, use a DMA heap fd) */
    int heap_fd = open("/dev/dma_heap/linux,cma", O_RDWR);
    if (heap_fd >= 0) {
        struct { uint64_t len; uint32_t fd; uint32_t fd_flags; uint64_t heap_flags; } alloc = {0};
        alloc.len = 1024 * 1024;
        alloc.fd_flags = O_CLOEXEC | O_RDWR;
        ret = ioctl(heap_fd, _IOWR('H', 0x0, typeof(alloc)), &alloc);
        if (ret == 0) {
            struct rknpu_mem_create mc2 = {0};
            mc2.handle = alloc.fd;  /* import mode */
            mc2.size = 1024 * 1024;
            ret = ioctl(fd, _IOWR('R', RKNPU_MEM_CREATE, struct rknpu_mem_create), &mc2);
            if (ret < 0) {
                printf("IMPORT (handle>0): FAILED errno=%d (%s)\n", errno, strerror(errno));
            } else {
                printf("IMPORT (handle>0): OK\n");
                printf("  dma_addr=0x%llx\n", (unsigned long long)mc2.dma_addr);
                struct rknpu_mem_destroy md2 = {0};
                md2.handle = mc2.handle;
                md2.obj_addr = mc2.obj_addr;
                ioctl(fd, _IOWR('R', RKNPU_MEM_DESTROY, struct rknpu_mem_destroy), &md2);
            }
            close(alloc.fd);
        }
        close(heap_fd);
    }

    close(fd);
    return 0;
}
