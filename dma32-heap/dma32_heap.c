// SPDX-License-Identifier: GPL-2.0
/*
 * DMA32 Heap - Allocates memory below 4GB for 32-bit DMA devices
 * Based on system_heap.c but with __GFP_DMA32 flag
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/iosys-map.h>

#define LOW_ORDER_GFP (GFP_KERNEL | __GFP_ZERO | __GFP_DMA32)
#define HIGH_ORDER_GFP (GFP_KERNEL | __GFP_ZERO | __GFP_DMA32 | __GFP_COMP | __GFP_NOWARN | __GFP_NORETRY)

static unsigned int orders[] = {8, 4, 0};
#define NUM_ORDERS ARRAY_SIZE(orders)

struct dma32_heap_buffer {
    struct dma_heap *heap;
    struct list_head attachments;
    struct mutex lock;
    unsigned long len;
    struct sg_table sg_table;
    int vmap_cnt;
    void *vaddr;
};

struct dma32_heap_attachment {
    struct device *dev;
    struct sg_table *table;
    struct list_head list;
    bool mapped;
};

static int dma32_heap_attach(struct dma_buf *dmabuf,
                             struct dma_buf_attachment *attachment)
{
    struct dma32_heap_buffer *buffer = dmabuf->priv;
    struct dma32_heap_attachment *a;
    struct sg_table *table;
    struct scatterlist *sg, *new_sg;
    int ret, i;

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;

    table = kzalloc(sizeof(*table), GFP_KERNEL);
    if (!table) {
        kfree(a);
        return -ENOMEM;
    }

    ret = sg_alloc_table(table, buffer->sg_table.orig_nents, GFP_KERNEL);
    if (ret) {
        kfree(table);
        kfree(a);
        return ret;
    }

    new_sg = table->sgl;
    for_each_sgtable_sg(&buffer->sg_table, sg, i) {
        sg_set_page(new_sg, sg_page(sg), sg->length, sg->offset);
        new_sg = sg_next(new_sg);
    }

    a->table = table;
    a->dev = attachment->dev;
    INIT_LIST_HEAD(&a->list);
    a->mapped = false;

    attachment->priv = a;

    mutex_lock(&buffer->lock);
    list_add(&a->list, &buffer->attachments);
    mutex_unlock(&buffer->lock);

    return 0;
}

static void dma32_heap_detach(struct dma_buf *dmabuf,
                              struct dma_buf_attachment *attachment)
{
    struct dma32_heap_buffer *buffer = dmabuf->priv;
    struct dma32_heap_attachment *a = attachment->priv;

    mutex_lock(&buffer->lock);
    list_del(&a->list);
    mutex_unlock(&buffer->lock);

    sg_free_table(a->table);
    kfree(a->table);
    kfree(a);
}

static struct sg_table *dma32_heap_map_dma_buf(struct dma_buf_attachment *attachment,
                                               enum dma_data_direction direction)
{
    struct dma32_heap_attachment *a = attachment->priv;
    struct sg_table *table = a->table;
    int ret;

    ret = dma_map_sgtable(attachment->dev, table, direction, 0);
    if (ret)
        return ERR_PTR(ret);

    a->mapped = true;
    return table;
}

static void dma32_heap_unmap_dma_buf(struct dma_buf_attachment *attachment,
                                     struct sg_table *table,
                                     enum dma_data_direction direction)
{
    struct dma32_heap_attachment *a = attachment->priv;

    a->mapped = false;
    dma_unmap_sgtable(attachment->dev, table, direction, 0);
}

static void dma32_heap_dma_buf_release(struct dma_buf *dmabuf)
{
    struct dma32_heap_buffer *buffer = dmabuf->priv;
    struct sg_table *table = &buffer->sg_table;
    struct scatterlist *sg;
    int i;

    for_each_sgtable_sg(table, sg, i) {
        struct page *page = sg_page(sg);
        __free_pages(page, compound_order(page));
    }
    sg_free_table(table);
    kfree(buffer);
}

static int dma32_heap_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
    struct dma32_heap_buffer *buffer = dmabuf->priv;
    struct sg_table *table = &buffer->sg_table;
    unsigned long addr = vma->vm_start;
    struct scatterlist *sg;
    int i, ret;

    for_each_sgtable_sg(table, sg, i) {
        struct page *page = sg_page(sg);
        unsigned long remainder = vma->vm_end - addr;
        unsigned long len = sg->length;

        if (len > remainder)
            len = remainder;

        ret = remap_pfn_range(vma, addr, page_to_pfn(page), len, vma->vm_page_prot);
        if (ret)
            return ret;
        addr += len;
        if (addr >= vma->vm_end)
            break;
    }
    return 0;
}

static int dma32_heap_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
    struct dma32_heap_buffer *buffer = dmabuf->priv;
    struct sg_table *table = &buffer->sg_table;
    unsigned int npages = PAGE_ALIGN(buffer->len) >> PAGE_SHIFT;
    struct page **pages;
    struct page **tmp;
    struct scatterlist *sg;
    pgprot_t pgprot = PAGE_KERNEL;
    void *vaddr;
    int i;

    mutex_lock(&buffer->lock);
    if (buffer->vmap_cnt) {
        buffer->vmap_cnt++;
        iosys_map_set_vaddr(map, buffer->vaddr);
        mutex_unlock(&buffer->lock);
        return 0;
    }

    pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
    if (!pages) {
        mutex_unlock(&buffer->lock);
        return -ENOMEM;
    }

    tmp = pages;
    for_each_sgtable_sg(table, sg, i) {
        unsigned int npages_this = sg->length >> PAGE_SHIFT;
        struct page *page = sg_page(sg);
        unsigned int j;

        for (j = 0; j < npages_this; j++)
            *(tmp++) = page++;
    }

    vaddr = vmap(pages, npages, VM_MAP, pgprot);
    kvfree(pages);

    if (!vaddr) {
        mutex_unlock(&buffer->lock);
        return -ENOMEM;
    }

    buffer->vaddr = vaddr;
    buffer->vmap_cnt++;
    iosys_map_set_vaddr(map, vaddr);
    mutex_unlock(&buffer->lock);

    return 0;
}

static void dma32_heap_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
    struct dma32_heap_buffer *buffer = dmabuf->priv;

    mutex_lock(&buffer->lock);
    if (WARN_ON(buffer->vmap_cnt == 0)) {
        mutex_unlock(&buffer->lock);
        return;
    }

    buffer->vmap_cnt--;
    if (buffer->vmap_cnt == 0) {
        vunmap(buffer->vaddr);
        buffer->vaddr = NULL;
    }
    mutex_unlock(&buffer->lock);
}

static const struct dma_buf_ops dma32_heap_buf_ops = {
    .attach = dma32_heap_attach,
    .detach = dma32_heap_detach,
    .map_dma_buf = dma32_heap_map_dma_buf,
    .unmap_dma_buf = dma32_heap_unmap_dma_buf,
    .release = dma32_heap_dma_buf_release,
    .mmap = dma32_heap_mmap,
    .vmap = dma32_heap_vmap,
    .vunmap = dma32_heap_vunmap,
};

static struct page *alloc_largest_available_dma32(unsigned long size, unsigned int max_order)
{
    struct page *page;
    gfp_t gfp;
    int i;

    for (i = 0; i < NUM_ORDERS; i++) {
        if (size < (PAGE_SIZE << orders[i]))
            continue;
        if (max_order < orders[i])
            continue;

        gfp = (orders[i] > 0) ? HIGH_ORDER_GFP : LOW_ORDER_GFP;
        page = alloc_pages(gfp, orders[i]);
        if (!page)
            continue;
        
        /* Verify allocation is below 4GB */
        if (page_to_phys(page) >= 0x100000000ULL) {
            pr_warn("DMA32 Heap: allocation at 0x%llx rejected (>4GB)\n",
                    (unsigned long long)page_to_phys(page));
            __free_pages(page, orders[i]);
            continue;
        }
        return page;
    }
    return NULL;
}

static struct dma_buf *dma32_heap_allocate(struct dma_heap *heap,
                                           unsigned long len,
                                           u32 fd_flags,
                                           u64 heap_flags)
{
    struct dma32_heap_buffer *buffer;
    DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
    unsigned long size_remaining = len;
    unsigned int max_order = orders[0];
    struct dma_buf *dmabuf;
    struct sg_table *table;
    struct scatterlist *sg;
    struct list_head pages;
    struct page *page, *tmp_page;
    int i, ret = -ENOMEM;

    buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
    if (!buffer)
        return ERR_PTR(-ENOMEM);

    INIT_LIST_HEAD(&buffer->attachments);
    mutex_init(&buffer->lock);
    buffer->heap = heap;
    buffer->len = len;

    INIT_LIST_HEAD(&pages);
    i = 0;
    while (size_remaining > 0) {
        if (fatal_signal_pending(current)) {
            ret = -EINTR;
            goto free_buffer;
        }

        page = alloc_largest_available_dma32(size_remaining, max_order);
        if (!page)
            goto free_buffer;

        list_add_tail(&page->lru, &pages);
        size_remaining -= page_size(page);
        max_order = compound_order(page);
        i++;
    }

    table = &buffer->sg_table;
    if (sg_alloc_table(table, i, GFP_KERNEL))
        goto free_buffer;

    sg = table->sgl;
    list_for_each_entry_safe(page, tmp_page, &pages, lru) {
        sg_set_page(sg, page, page_size(page), 0);
        sg = sg_next(sg);
        list_del(&page->lru);
    }

    exp_info.exp_name = dma_heap_get_name(heap);
    exp_info.ops = &dma32_heap_buf_ops;
    exp_info.size = buffer->len;
    exp_info.flags = fd_flags;
    exp_info.priv = buffer;
    dmabuf = dma_buf_export(&exp_info);
    if (IS_ERR(dmabuf)) {
        ret = PTR_ERR(dmabuf);
        goto free_pages;
    }

    return dmabuf;

free_pages:
    for_each_sgtable_sg(table, sg, i) {
        struct page *p = sg_page(sg);
        if (p)
            __free_pages(p, compound_order(p));
    }
    sg_free_table(table);
free_buffer:
    list_for_each_entry_safe(page, tmp_page, &pages, lru)
        __free_pages(page, compound_order(page));
    kfree(buffer);
    return ERR_PTR(ret);
}

static const struct dma_heap_ops dma32_heap_ops = {
    .allocate = dma32_heap_allocate,
};

static struct dma_heap *dma32_heap;

static int __init dma32_heap_init(void)
{
    struct dma_heap_export_info exp_info;

    exp_info.name = "dma32";
    exp_info.ops = &dma32_heap_ops;
    exp_info.priv = NULL;

    dma32_heap = dma_heap_add(&exp_info);
    if (IS_ERR(dma32_heap))
        return PTR_ERR(dma32_heap);

    pr_info("DMA32 Heap: registered (allocations below 4GB)\n");
    return 0;
}

static void __exit dma32_heap_exit(void)
{
    pr_info("DMA32 Heap: unloaded\n");
}

module_init(dma32_heap_init);
module_exit(dma32_heap_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DMA32 Heap - allocates below 4GB");
MODULE_AUTHOR("RKNPU DKMS Project");

/* Kernel 6.x+ requires explicit namespace imports */
MODULE_IMPORT_NS("DMA_BUF");
MODULE_IMPORT_NS("DMA_BUF_HEAP");
