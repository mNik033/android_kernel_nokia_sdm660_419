// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#include <linux/dma-buf.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <asm/barrier.h>

struct msm_iommu_map {
	struct device *dev;
	struct msm_iommu_data *data;
	struct list_head data_node;
	struct list_head dev_node;
	struct scatterlist *sgl;
	struct scatterlist inline_sg;
	dma_addr_t buf_start_addr;
	unsigned long attrs;
	enum dma_data_direction dir;
	unsigned int nents;
	int refcount;
};

static struct msm_iommu_map *msm_iommu_map_lookup(struct msm_iommu_data *data,
						  struct device *dev)
{
	struct msm_iommu_map *map;

	list_for_each_entry(map, &data->map_list, data_node) {
		if (map->dev == dev)
			return map;
	}

	return NULL;
}

static struct scatterlist *clone_sgl(struct scatterlist *sg, int nents)
{
	struct scatterlist *next, *s;
	int i;
	struct sg_table table;

	if (sg_alloc_table(&table, nents, GFP_KERNEL))
		return NULL;
	next = table.sgl;
	for_each_sg(sg, s, nents, i) {
		*next = *s;
		next = sg_next(next);
	}
	return table.sgl;
}

static void msm_iommu_map_free(struct msm_iommu_map *map)
{
	list_del(&map->data_node);
	list_del(&map->dev_node);
	/* Skip an additional cache maintenance on the dma unmap path */
	map->attrs |= DMA_ATTR_SKIP_CPU_SYNC;
	dma_unmap_sg_attrs(map->dev, map->sgl, map->nents, map->dir,
			   map->attrs);
	if (map->sgl != &map->inline_sg) {
		struct sg_table table = {
			.sgl = map->sgl,
			.nents = map->nents,
			.orig_nents = map->nents,
		};

		sg_free_table(&table);
	}
	kfree(map);
}

/*
 * We are not taking a reference to the dma_buf here. It is expected that
 * clients hold reference to the dma_buf until they are done with mapping and
 * unmapping.
 */
int msm_dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
			 enum dma_data_direction dir, struct dma_buf *dmabuf,
			 unsigned long attrs)
{
	int not_lazy = !!(attrs & DMA_ATTR_NO_DELAYED_UNMAP);
	struct msm_iommu_data *data;
	struct msm_iommu_map *map;
	int ret;

	if (IS_ERR_OR_NULL(dev)) {
		pr_err("%s: dev pointer is invalid\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(sg) || nents <= 0) {
		pr_err("%s: sg table pointer is invalid\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(dmabuf)) {
		pr_err("%s: dma_buf pointer is invalid\n", __func__);
		return -EINVAL;
	}

	data = dmabuf->priv;

	mutex_lock(&dev->iommu_map_lock);
	mutex_lock(&data->lock);
	map = msm_iommu_map_lookup(data, dev);
	if (map) {
		if (nents == map->nents &&
		    dir == map->dir &&
		    (attrs & ~DMA_ATTR_SKIP_CPU_SYNC) ==
		    (map->attrs & ~DMA_ATTR_SKIP_CPU_SYNC) &&
		    sg_phys(sg) == map->buf_start_addr) {
			struct scatterlist *sg_tmp = sg;
			struct scatterlist *map_sg;
			int i;

			for_each_sg(map->sgl, map_sg, nents, i) {
				sg_dma_address(sg_tmp) = sg_dma_address(map_sg);
				sg_dma_len(sg_tmp) = sg_dma_len(map_sg);
				if (sg_dma_len(map_sg) == 0)
					break;

				sg_tmp = sg_next(sg_tmp);
				if (!sg_tmp)
					break;
			}

			map->refcount++;

			if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))
				dma_sync_sg_for_device(dev, map->sgl,
						       map->nents, map->dir);

			if (is_device_dma_coherent(dev))
				/*
				 * Ensure all outstanding changes for coherent
				 * buffers are applied to the cache before any
				 * DMA occurs.
				 */
				dmb(ish);
			ret = nents;
		} else {
			bool start_diff = (sg_phys(sg) != map->buf_start_addr);

			dev_err(dev, "lazy map request differs:\n"
				"req dir:%d, original dir:%d\n"
				"req nents:%d, original nents:%d\n"
				"req map attrs:%lu, original map attrs:%lu\n"
				"req buffer start address differs:%d\n",
				dir, map->dir, nents,
				map->nents, attrs, map->attrs,
				start_diff);
			ret = -EINVAL;
		}
	} else {
		map = kmalloc(sizeof(*map), GFP_KERNEL);
		if (!map) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		ret = dma_map_sg_attrs(dev, sg, nents, dir, attrs);
		if (!ret) {
			kfree(map);
			goto out_unlock;
		}

		if (nents == 1) {
			map->inline_sg = *sg;
			map->sgl = &map->inline_sg;
		} else {
			map->sgl = clone_sgl(sg, nents);
			if (!map->sgl) {
				dma_unmap_sg_attrs(dev, sg, nents, dir,
						   attrs | DMA_ATTR_SKIP_CPU_SYNC);
				kfree(map);
				ret = -ENOMEM;
				goto out_unlock;
			}
		}

		map->data = data;
		map->dev = dev;
		map->dir = dir;
		map->nents = nents;
		map->attrs = attrs;
		map->buf_start_addr = sg_phys(sg);
		map->refcount = 2 - not_lazy;
		list_add(&map->data_node, &data->map_list);
		list_add(&map->dev_node, &dev->iommu_map_list);
	}
out_unlock:
	mutex_unlock(&data->lock);
	mutex_unlock(&dev->iommu_map_lock);

	return ret;
}
EXPORT_SYMBOL(msm_dma_map_sg_attrs);

void msm_dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sgl,
			    int nents, enum dma_data_direction dir,
			    struct dma_buf *dmabuf, unsigned long attrs)
{
	struct msm_iommu_data *data;
	struct msm_iommu_map *map;

	if (IS_ERR_OR_NULL(dev) || IS_ERR_OR_NULL(dmabuf))
		return;

	data = dmabuf->priv;

	mutex_lock(&dev->iommu_map_lock);
	mutex_lock(&data->lock);
	map = msm_iommu_map_lookup(data, dev);
	if (!map) {
		WARN(1, "%s: (%p) was never mapped for device %p\n",
		     __func__, dmabuf, dev);
		goto out_unlock;
	}

	if (dir != map->dir)
		WARN(1, "%s: (%pK) dir:%d differs from original dir:%d\n",
		     __func__, dmabuf, dir, map->dir);

	if (attrs && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		dma_sync_sg_for_cpu(dev, map->sgl, map->nents, dir);

	map->attrs = attrs;
	if (!--map->refcount)
		msm_iommu_map_free(map);
out_unlock:
	mutex_unlock(&data->lock);
	mutex_unlock(&dev->iommu_map_lock);
}
EXPORT_SYMBOL(msm_dma_unmap_sg_attrs);

void msm_dma_unmap_all_for_dev(struct device *dev)
{
	struct msm_iommu_map *map, *tmp;

	mutex_lock(&dev->iommu_map_lock);
	list_for_each_entry_safe(map, tmp, &dev->iommu_map_list, dev_node) {
		struct msm_iommu_data *data = map->data;

		mutex_lock(&data->lock);
		msm_iommu_map_free(map);
		mutex_unlock(&data->lock);
	}
	mutex_unlock(&dev->iommu_map_lock);
}

/*
 * Only to be called by ION code when a buffer is freed
 */
void msm_dma_buf_freed(struct msm_iommu_data *data)
{
	struct msm_iommu_map *map, *tmp;

	for (;;) {
		struct device *retry_dev = NULL;

		mutex_lock(&data->lock);
		list_for_each_entry_safe(map, tmp, &data->map_list, data_node) {
			struct device *dev = map->dev;

			if (!mutex_trylock(&dev->iommu_map_lock)) {
				retry_dev = get_device(dev);
				break;
			}

			msm_iommu_map_free(map);
			mutex_unlock(&dev->iommu_map_lock);
		}
		mutex_unlock(&data->lock);

		if (!retry_dev)
			break;

		/* Wait for the concurrent dev->iommu_map_lock holder to finish */
		mutex_lock(&retry_dev->iommu_map_lock);
		mutex_unlock(&retry_dev->iommu_map_lock);
		put_device(retry_dev);
	}
}
