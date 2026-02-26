// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 */

#include "rknpu_debugger.h"
#include "rknpu_mm.h"

int rknpu_mm_create(unsigned int mem_size, unsigned int chunk_size,
		    struct rknpu_mm **mm)
{
	unsigned int num_of_longs;
	int ret = -EINVAL;

	if (WARN_ON(mem_size < chunk_size))
		return -EINVAL;
	if (WARN_ON(mem_size == 0))
		return -EINVAL;
	if (WARN_ON(chunk_size == 0))
		return -EINVAL;

	*mm = kzalloc(sizeof(struct rknpu_mm), GFP_KERNEL);
	if (!(*mm))
		return -ENOMEM;

	(*mm)->chunk_size = chunk_size;
	(*mm)->total_chunks = mem_size / chunk_size;
	(*mm)->free_chunks = (*mm)->total_chunks;

	num_of_longs = BITS_TO_LONGS((*mm)->total_chunks);

	(*mm)->bitmap = kcalloc(num_of_longs, sizeof(long), GFP_KERNEL);
	if (!(*mm)->bitmap) {
		ret = -ENOMEM;
		goto free_mm;
	}

	mutex_init(&(*mm)->lock);

	LOG_DEBUG("total_chunks: %d, bitmap: %p\n", (*mm)->total_chunks,
		  (*mm)->bitmap);

	return 0;

free_mm:
	kfree(mm);
	return ret;
}

void rknpu_mm_destroy(struct rknpu_mm *mm)
{
	if (mm != NULL) {
		mutex_destroy(&mm->lock);
		kfree(mm->bitmap);
		kfree(mm);
	}
}

int rknpu_mm_alloc(struct rknpu_mm *mm, unsigned int size,
		   struct rknpu_mm_obj **mm_obj)
{
	unsigned int num_chunks;
	unsigned long found;

	if (size == 0)
		return -EINVAL;

	num_chunks = DIV_ROUND_UP(size, mm->chunk_size);
	if (num_chunks > mm->total_chunks)
		return -ENOMEM;

	*mm_obj = kzalloc(sizeof(struct rknpu_mm_obj), GFP_KERNEL);
	if (!(*mm_obj))
		return -ENOMEM;

	mutex_lock(&mm->lock);

	found = bitmap_find_next_zero_area(mm->bitmap, mm->total_chunks,
					   0, num_chunks, 0);
	if (found >= mm->total_chunks) {
		mutex_unlock(&mm->lock);
		kfree(*mm_obj);
		return -ENOMEM;
	}

	bitmap_set(mm->bitmap, found, num_chunks);
	mm->free_chunks -= num_chunks;

	mutex_unlock(&mm->lock);

	(*mm_obj)->range_start = found;
	(*mm_obj)->range_end = found + num_chunks - 1;

	LOG_DEBUG("mm allocate, mm_obj: %p, range_start: %d, range_end: %d\n",
		  *mm_obj, (*mm_obj)->range_start, (*mm_obj)->range_end);

	return 0;
}

int rknpu_mm_free(struct rknpu_mm *mm, struct rknpu_mm_obj *mm_obj)
{
	unsigned int count;

	/* Act like kfree when trying to free a NULL object */
	if (!mm_obj)
		return 0;

	LOG_DEBUG("mm free, mem_obj: %p, range_start: %d, range_end: %d\n",
		  mm_obj, mm_obj->range_start, mm_obj->range_end);

	count = mm_obj->range_end - mm_obj->range_start + 1;

	mutex_lock(&mm->lock);
	bitmap_clear(mm->bitmap, mm_obj->range_start, count);
	mm->free_chunks += count;
	mutex_unlock(&mm->lock);

	kfree(mm_obj);

	return 0;
}

int rknpu_mm_dump(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	struct rknpu_mm *mm = NULL;
	int cur = 0, rbot = 0, rtop = 0;
	size_t ret = 0;
	char buf[64];
	size_t size = sizeof(buf);
	int seg_chunks = 32, seg_id = 0;
	int free_size = 0;
	int i = 0;

	mm = rknpu_dev->sram_mm;
	if (mm == NULL)
		return 0;

	seq_printf(m, "SRAM bitmap: \"*\" - used, \".\" - free (1bit = %dKB)\n",
		   mm->chunk_size / 1024);

	rbot = cur = find_first_bit(mm->bitmap, mm->total_chunks);
	for (i = 0; i < cur; ++i) {
		ret += scnprintf(buf + ret, size - ret, ".");
		if (ret >= seg_chunks) {
			seq_printf(m, "[%03d] [%s]\n", seg_id++, buf);
			ret = 0;
		}
	}
	while (cur < mm->total_chunks) {
		rtop = cur;
		cur = find_next_bit(mm->bitmap, mm->total_chunks, cur + 1);
		if (cur < mm->total_chunks && cur <= rtop + 1)
			continue;

		for (i = rbot; i <= rtop; ++i) {
			ret += scnprintf(buf + ret, size - ret, "*");
			if (ret >= seg_chunks) {
				seq_printf(m, "[%03d] [%s]\n", seg_id++, buf);
				ret = 0;
			}
		}

		for (i = rtop + 1; i < cur; ++i) {
			ret += scnprintf(buf + ret, size - ret, ".");
			if (ret >= seg_chunks) {
				seq_printf(m, "[%03d] [%s]\n", seg_id++, buf);
				ret = 0;
			}
		}

		rbot = cur;
	}

	if (ret > 0)
		seq_printf(m, "[%03d] [%s]\n", seg_id++, buf);

	free_size = mm->free_chunks * mm->chunk_size;
	seq_printf(m, "SRAM total size: %d, used: %d, free: %d\n",
		   rknpu_dev->sram_size, rknpu_dev->sram_size - free_size,
		   free_size);

	return 0;
}
