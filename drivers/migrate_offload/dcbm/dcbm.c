// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA Core Batch Migrator (DCBM)
 *
 * Uses DMAEngine memcpy channels to offload batch folio copies during
 * page migration. Reference driver meant for testing the offload
 * infrastructure.
 *
 * Copyright (C) 2024-26 Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/migrate.h>
#include <linux/migrate_copy_offload.h>
#include <linux/rwsem.h>
#include <linux/workqueue.h>

#define MAX_DMA_CHANNELS	16
#define MAX_DMA_BUSES		4

static atomic_long_t folios_migrated;
static atomic_long_t folios_failures;

static bool offloading_enabled;
static unsigned int nr_dma_channels = 1;
static DEFINE_MUTEX(dcbm_mutex);

/*
 * Channels are held while offloading is enabled; batches take dcbm_chan_sem
 * for read and set updates take it for write. Bus notifiers drop a provider's
 * channels before unbind frees them and keep the device on dcbm_dying_devs
 * until unbind completes.
 */
static DECLARE_RWSEM(dcbm_chan_sem);
static struct dma_chan *dcbm_chans[MAX_DMA_CHANNELS];
static unsigned int dcbm_nr_chans;
static unsigned int dcbm_nr_want;
static atomic_t dcbm_next_chan;

struct dcbm_dying {
	struct list_head node;
	struct device *dev;
};

static LIST_HEAD(dcbm_dying_devs);

/* Watched buses, under dcbm_mutex; notifiers stay until module exit */
static const struct bus_type *dcbm_buses[MAX_DMA_BUSES];
static struct notifier_block dcbm_bus_nbs[MAX_DMA_BUSES];
static unsigned int dcbm_nr_buses;

static void dcbm_fill_work_fn(struct work_struct *work);
static DECLARE_WORK(dcbm_fill_work, dcbm_fill_work_fn);

struct dma_work {
	struct dma_chan *chan;
	struct completion done;
	atomic_t pending;
	struct sg_table *src_sgt;
	struct sg_table *dst_sgt;
	bool mapped;
	bool queued; /* a descriptor was submitted */
	bool failed;
};

static void dma_completion_callback(void *data)
{
	struct dma_work *work = data;

	if (atomic_dec_and_test(&work->pending))
		complete(&work->done);
}

static int setup_sg_tables(struct dma_work *work, struct list_head **src_pos,
			   struct list_head **dst_pos, int nr)
{
	struct scatterlist *sg_src, *sg_dst;
	struct device *dev;
	int i, ret;

	work->src_sgt = kmalloc_obj(*work->src_sgt, GFP_KERNEL);
	if (!work->src_sgt)
		return -ENOMEM;
	work->dst_sgt = kmalloc_obj(*work->dst_sgt, GFP_KERNEL);
	if (!work->dst_sgt) {
		ret = -ENOMEM;
		goto err_free_src;
	}

	ret = sg_alloc_table(work->src_sgt, nr, GFP_KERNEL);
	if (ret)
		goto err_free_dst;
	ret = sg_alloc_table(work->dst_sgt, nr, GFP_KERNEL);
	if (ret)
		goto err_free_src_table;

	sg_src = work->src_sgt->sgl;
	sg_dst = work->dst_sgt->sgl;
	for (i = 0; i < nr; i++) {
		struct folio *src = list_entry(*src_pos, struct folio, lru);
		struct folio *dst = list_entry(*dst_pos, struct folio, lru);

		sg_set_folio(sg_src, src, folio_size(src), 0);
		sg_set_folio(sg_dst, dst, folio_size(dst), 0);

		*src_pos = (*src_pos)->next;
		*dst_pos = (*dst_pos)->next;

		if (i < nr - 1) {
			sg_src = sg_next(sg_src);
			sg_dst = sg_next(sg_dst);
		}
	}

	dev = dmaengine_get_dma_device(work->chan);
	if (!dev) {
		ret = -ENODEV;
		goto err_free_dst_table;
	}
	ret = dma_map_sgtable(dev, work->src_sgt, DMA_TO_DEVICE,
			      DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_NO_KERNEL_MAPPING);
	if (ret)
		goto err_free_dst_table;
	ret = dma_map_sgtable(dev, work->dst_sgt, DMA_FROM_DEVICE,
			      DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_NO_KERNEL_MAPPING);
	if (ret)
		goto err_unmap_src;

	/*
	 * TODO: IOMMU may merge segments unevenly on the two sides, fall back
	 * bail to CPU copy. In practice, I have not observed merging in tests.
	 * Handling unequal nents is left for follow-up.
	 */
	if (work->src_sgt->nents != work->dst_sgt->nents) {
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	work->mapped = true;
	return 0;

err_unmap_dst:
	dma_unmap_sgtable(dev, work->dst_sgt, DMA_FROM_DEVICE,
			  DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_NO_KERNEL_MAPPING);
err_unmap_src:
	dma_unmap_sgtable(dev, work->src_sgt, DMA_TO_DEVICE,
			  DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_NO_KERNEL_MAPPING);
err_free_dst_table:
	sg_free_table(work->dst_sgt);
err_free_src_table:
	sg_free_table(work->src_sgt);
err_free_dst:
	kfree(work->dst_sgt);
	work->dst_sgt = NULL;
err_free_src:
	kfree(work->src_sgt);
	work->src_sgt = NULL;
	return ret;
}

static void cleanup_dma_work(struct dma_work *works, int actual_channels)
{
	struct device *dev;
	int i;

	if (!works)
		return;

	for (i = 0; i < actual_channels; i++) {
		if (!works[i].chan)
			continue;

		dev = dmaengine_get_dma_device(works[i].chan);

		/* Stop a failed channel before its buffers are unmapped. */
		if (works[i].failed && works[i].queued)
			dmaengine_terminate_sync(works[i].chan);

		if (dev && works[i].mapped) {
			if (works[i].src_sgt) {
				dma_unmap_sgtable(dev, works[i].src_sgt,
						  DMA_TO_DEVICE,
						  DMA_ATTR_SKIP_CPU_SYNC |
						  DMA_ATTR_NO_KERNEL_MAPPING);
				sg_free_table(works[i].src_sgt);
				kfree(works[i].src_sgt);
			}
			if (works[i].dst_sgt) {
				dma_unmap_sgtable(dev, works[i].dst_sgt,
						  DMA_FROM_DEVICE,
						  DMA_ATTR_SKIP_CPU_SYNC |
						  DMA_ATTR_NO_KERNEL_MAPPING);
				sg_free_table(works[i].dst_sgt);
				kfree(works[i].dst_sgt);
			}
		}
	}
}

static int submit_dma_transfers(struct dma_work *work)
{
	struct scatterlist *sg_src, *sg_dst;
	struct dma_async_tx_descriptor *tx;
	unsigned long flags = DMA_CTRL_ACK;
	dma_cookie_t cookie;
	bool no_order, last;
	int i;

	/* Last-descriptor callback covers earlier ones only on ordered channels. */
	no_order = dma_has_cap(DMA_COMPLETION_NO_ORDER,
			       work->chan->device->cap_mask);
	atomic_set(&work->pending, 1);

	sg_src = work->src_sgt->sgl;
	sg_dst = work->dst_sgt->sgl;
	for_each_sgtable_dma_sg(work->src_sgt, sg_src, i) {
		last = i == work->src_sgt->nents - 1;
		if (last || no_order)
			flags |= DMA_PREP_INTERRUPT;

		tx = dmaengine_prep_dma_memcpy(work->chan,
					       sg_dma_address(sg_dst),
					       sg_dma_address(sg_src),
					       sg_dma_len(sg_src), flags);
		if (!tx)
			return -EIO;

		if (last || no_order) {
			if (!last)
				atomic_inc(&work->pending);
			tx->callback = dma_completion_callback;
			tx->callback_param = work;
		}

		cookie = dmaengine_submit(tx);
		if (dma_submit_error(cookie))
			return -EIO;
		work->queued = true;
		sg_dst = sg_next(sg_dst);
	}
	return 0;
}

static void dcbm_replace_channel(struct dma_chan *bad);

/**
 * folios_copy_dma - copy a batch of folios via DMA memcpy
 * @dst_list: destination folio list
 * @src_list: source folio list
 * @nr_folios: number of folios in each list
 *
 * Return: 0 on success, negative errno on failure.
 */
static int folios_copy_dma(struct list_head *dst_list,
			   struct list_head *src_list, unsigned int nr_folios)
{
	struct folio *dst;
	struct dma_work *works = NULL;
	struct list_head *src_pos = src_list->next;
	struct list_head *dst_pos = dst_list->next;
	int i, folios_per_chan, ret = 0;
	unsigned int first, max_channels;
	int actual_channels = 0;
	unsigned long left;

	down_read(&dcbm_chan_sem);
	if (!dcbm_nr_chans) {
		ret = -ENODEV;
		goto err_cleanup;
	}
	max_channels = min(dcbm_nr_chans, nr_folios);

	works = kcalloc(max_channels, sizeof(*works), GFP_KERNEL);
	if (!works) {
		ret = -ENOMEM;
		goto err_cleanup;
	}

	/* Spread batches over the channels round-robin. */
	first = (unsigned int)atomic_inc_return(&dcbm_next_chan);
	for (i = 0; i < max_channels; i++) {
		works[i].chan = dcbm_chans[(first + i) % dcbm_nr_chans];
		init_completion(&works[i].done);
	}
	actual_channels = max_channels;

	for (i = 0; i < actual_channels; i++) {
		folios_per_chan = nr_folios * (i + 1) / actual_channels -
				(nr_folios * i) / actual_channels;
		if (folios_per_chan == 0)
			continue;

		ret = setup_sg_tables(&works[i], &src_pos, &dst_pos,
				      folios_per_chan);
		if (ret)
			goto err_cleanup;
	}

	for (i = 0; i < actual_channels; i++) {
		if (!works[i].mapped)
			continue;
		ret = submit_dma_transfers(&works[i]);
		if (ret) {
			works[i].failed = true;
			break;
		}
	}

	/*
	 * Shared channels carry other batches; finish submitted works and
	 * terminate only channels that failed or timed out.
	 */
	for (i = 0; i < actual_channels; i++) {
		if (works[i].queued && !works[i].failed)
			dma_async_issue_pending(works[i].chan);
	}

	left = msecs_to_jiffies(10000);
	for (i = 0; i < actual_channels; i++) {
		if (!works[i].queued || works[i].failed)
			continue;
		left = wait_for_completion_timeout(&works[i].done, left);
		if (!left) {
			works[i].failed = true;
			ret = ret ?: -ETIMEDOUT;
		}
	}
	if (ret)
		goto err_cleanup;

	/*
	 * All folios copied; mark each dst with FOLIO_CONTENT_COPIED so
	 * __migrate_folio() skips the per-folio copy in the move phase.
	 */
	list_for_each_entry(dst, dst_list, lru)
		dst->migrate_info |= FOLIO_CONTENT_COPIED;

	cleanup_dma_work(works, actual_channels);
	kfree(works);
	up_read(&dcbm_chan_sem);

	atomic_long_add(nr_folios, &folios_migrated);
	return 0;

err_cleanup:
	pr_warn_ratelimited("dcbm: DMA copy failed (%d), falling back to CPU\n",
			    ret);
	cleanup_dma_work(works, actual_channels);
	up_read(&dcbm_chan_sem);
	for (i = 0; i < actual_channels; i++)
		if (works[i].failed)
			dcbm_replace_channel(works[i].chan);
	kfree(works);

	atomic_long_add(nr_folios, &folios_failures);
	return ret;
}

static const struct migrator dma_migrator = {
	.name = "DCBM",
	.offload_copy = folios_copy_dma,
	.owner = THIS_MODULE,
};

static unsigned long dcbm_reason_mask = MIGRATE_OFFLOAD_REASONS_ALLOWED;

static void dcbm_release_channels(void)
{
	struct dma_chan *chans[MAX_DMA_CHANNELS];
	unsigned int n;

	down_write(&dcbm_chan_sem);
	n = dcbm_nr_chans;
	memcpy(chans, dcbm_chans, n * sizeof(*chans));
	dcbm_nr_chans = 0;
	up_write(&dcbm_chan_sem);

	while (n)
		dma_release_channel(chans[--n]);
}

static int dcbm_bus_index(const struct bus_type *bus)
{
	unsigned int i;

	for (i = 0; i < dcbm_nr_buses; i++)
		if (dcbm_buses[i] == bus)
			return i;
	return -1;
}

/* @dev is the provider of @chan or a device below it */
static bool dcbm_below_provider(struct device *dev, struct dma_chan *chan)
{
	for (; dev; dev = dev->parent)
		if (dev == chan->device->dev)
			return true;
	return false;
}

/* Called with dcbm_mutex held and dcbm_chan_sem held for write */
static bool dcbm_chan_filter(struct dma_chan *chan, void *param)
{
	struct device *dev = chan->device->dev;
	struct dcbm_dying *d;

	if (dcbm_bus_index(dev->bus) < 0)
		return false;
	list_for_each_entry(d, &dcbm_dying_devs, node)
		if (dcbm_below_provider(d->dev, chan))
			return false;
	return true;
}

/* Set by the scan; channel devices are children of their provider */
static const struct class *dcbm_chan_class;

static int dcbm_is_chan_dev(struct device *dev, void *data)
{
	const struct class *class = READ_ONCE(dcbm_chan_class);

	return class && dev->class == class;
}

/* @dev or a device above it has channels */
static bool dcbm_near_provider(struct device *dev)
{
	for (; dev; dev = dev->parent)
		if (device_for_each_child(dev, NULL, dcbm_is_chan_dev))
			return true;
	return false;
}

/*
 * Drop channels of providers at or above @dev; with @dying, block new ones.
 */
static void dcbm_drop_dev(struct device *dev, bool dying)
{
	struct dma_chan *gone[MAX_DMA_CHANNELS];
	struct dcbm_dying *d = NULL;
	unsigned int i = 0, n = 0;

	/* Most devices on a watched bus are not near a DMA provider. */
	if (!dcbm_near_provider(dev))
		return;

	if (dying) {
		d = kmalloc_obj(*d, GFP_KERNEL | __GFP_NOFAIL);
		d->dev = dev;
	}

	/* Waits for running batches and for a request in progress. */
	down_write(&dcbm_chan_sem);
	if (d)
		list_add(&d->node, &dcbm_dying_devs);
	while (i < dcbm_nr_chans) {
		if (dcbm_below_provider(dev, dcbm_chans[i])) {
			gone[n++] = dcbm_chans[i];
			dcbm_chans[i] = dcbm_chans[--dcbm_nr_chans];
		} else {
			i++;
		}
	}
	up_write(&dcbm_chan_sem);

	while (n)
		dma_release_channel(gone[--n]);
}

static void dcbm_forget_dev(struct device *dev)
{
	struct dcbm_dying *d, *tmp;
	bool found = false;

	down_read(&dcbm_chan_sem);
	list_for_each_entry(d, &dcbm_dying_devs, node) {
		if (d->dev == dev) {
			found = true;
			break;
		}
	}
	up_read(&dcbm_chan_sem);
	if (!found)
		return;

	down_write(&dcbm_chan_sem);
	list_for_each_entry_safe(d, tmp, &dcbm_dying_devs, node) {
		if (d->dev == dev) {
			list_del(&d->node);
			kfree(d);
		}
	}
	up_write(&dcbm_chan_sem);
}

/* BUS_NOTIFY_UNBIND_DRIVER precedes remove() and devres teardown. */
static int dcbm_bus_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct device *dev = data;

	switch (action) {
	case BUS_NOTIFY_UNBIND_DRIVER:
		dcbm_drop_dev(dev, true);
		break;
	case BUS_NOTIFY_DRIVER_NOT_BOUND:
		dcbm_drop_dev(dev, false);
		break;
	case BUS_NOTIFY_UNBOUND_DRIVER:
	case BUS_NOTIFY_DEL_DEVICE:
		dcbm_forget_dev(dev);
		fallthrough;
	case BUS_NOTIFY_BOUND_DRIVER:
		if (READ_ONCE(offloading_enabled))
			schedule_work(&dcbm_fill_work);
		break;
	}
	return NOTIFY_DONE;
}

struct dcbm_scan {
	struct device *devs[MAX_DMA_CHANNELS];
	unsigned int nr;
};

/* Collect the providers a request could pick from; take no channel */
static bool dcbm_scan_filter(struct dma_chan *chan, void *param)
{
	struct dcbm_scan *scan = param;
	struct device *dev = chan->device->dev;
	unsigned int i;

	WRITE_ONCE(dcbm_chan_class, dmaengine_chan_dev(chan)->class);
	for (i = 0; i < scan->nr; i++)
		if (scan->devs[i] == dev)
			return false;
	if (scan->nr < ARRAY_SIZE(scan->devs))
		scan->devs[scan->nr++] = get_device(dev);
	return false;
}

/* Watch the bus of @dev and of the devices below it */
static int dcbm_watch_dev(struct device *dev, void *data)
{
	const struct bus_type *bus = dev->bus;
	struct notifier_block *nb;

	if (bus && dcbm_bus_index(bus) < 0 && dcbm_nr_buses < MAX_DMA_BUSES) {
		nb = &dcbm_bus_nbs[dcbm_nr_buses];
		nb->notifier_call = dcbm_bus_notify;
		if (!bus_register_notifier(bus, nb))
			dcbm_buses[dcbm_nr_buses++] = bus;
	}
	return device_for_each_child(dev, NULL, dcbm_watch_dev);
}

/* Wait for an unbind on a bus watched from index *@old on */
static int dcbm_wait_unbind(struct device *dev, void *old)
{
	if (dev->bus && dcbm_bus_index(dev->bus) >= *(int *)old) {
		device_lock(dev);
		device_unlock(dev);
	}
	return device_for_each_child(dev, old, dcbm_wait_unbind);
}

/*
 * Watch provider and child buses; synchronize with any unbind already in
 * flight under device_lock before acquiring channels.
 */
static void dcbm_watch_buses(dma_cap_mask_t mask)
{
	struct dcbm_scan scan = {};
	int old = dcbm_nr_buses;
	unsigned int i;

	dma_request_channel(mask, dcbm_scan_filter, &scan);

	for (i = 0; i < scan.nr; i++)
		dcbm_watch_dev(scan.devs[i], NULL);
	for (i = 0; i < scan.nr; i++) {
		if (dcbm_nr_buses > old)
			dcbm_wait_unbind(scan.devs[i], &old);
		put_device(scan.devs[i]);
	}
}

/* Request channels until dcbm_nr_want are held */
static void dcbm_fill_channels(void)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;

	lockdep_assert_held(&dcbm_mutex);
	if (READ_ONCE(dcbm_nr_chans) >= dcbm_nr_want)
		return;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	dcbm_watch_buses(mask);

	/* An unbind waits for this, so it sees every channel taken here. */
	down_write(&dcbm_chan_sem);
	while (dcbm_nr_chans < dcbm_nr_want) {
		chan = dma_request_channel(mask, dcbm_chan_filter, NULL);
		if (!chan)
			break;
		dcbm_chans[dcbm_nr_chans++] = chan;
	}
	up_write(&dcbm_chan_sem);
}

static void dcbm_fill_work_fn(struct work_struct *work)
{
	mutex_lock(&dcbm_mutex);
	if (offloading_enabled)
		dcbm_fill_channels();
	mutex_unlock(&dcbm_mutex);
}

/*
 * Drop and release a failed channel and let fill work request a fresh one.
 * Runs inside migrate_offload SRCU read section, so do not take dcbm_mutex.
 */
static void dcbm_replace_channel(struct dma_chan *bad)
{
	unsigned int i;
	bool found = false;

	down_write(&dcbm_chan_sem);
	for (i = 0; i < dcbm_nr_chans; i++) {
		if (dcbm_chans[i] == bad) {
			dcbm_chans[i] = dcbm_chans[--dcbm_nr_chans];
			found = true;
			break;
		}
	}
	up_write(&dcbm_chan_sem);
	if (!found)
		return;

	dma_release_channel(bad);
	schedule_work(&dcbm_fill_work);
}

/* offloading: enable/disable DMA migration offload */
static int offloading_param_set(const char *val, const struct kernel_param *kp)
{
	bool enable;
	int ret;

	ret = kstrtobool(val, &enable);
	if (ret)
		return ret;

	mutex_lock(&dcbm_mutex);
	if (enable == offloading_enabled) {
		mutex_unlock(&dcbm_mutex);
		return 0;
	}
	if (enable) {
		dcbm_nr_want = READ_ONCE(nr_dma_channels);
		dcbm_fill_channels();
		if (!READ_ONCE(dcbm_nr_chans)) {
			ret = -ENODEV;
			goto out;
		}
		ret = migrate_offload_register(&dma_migrator,
					       READ_ONCE(dcbm_reason_mask));
		if (ret) {
			dcbm_release_channels();
			goto out;
		}
		WRITE_ONCE(offloading_enabled, true);
	} else {
		migrate_offload_unregister(&dma_migrator);
		dcbm_release_channels();
		WRITE_ONCE(offloading_enabled, false);
	}
out:
	mutex_unlock(&dcbm_mutex);
	return ret;
}

static int offloading_param_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%d\n", READ_ONCE(offloading_enabled));
}

static const struct kernel_param_ops offloading_param_ops = {
	.set = offloading_param_set,
	.get = offloading_param_get,
};
module_param_cb(offloading, &offloading_param_ops, NULL, 0644);
MODULE_PARM_DESC(offloading, "Enable DMA migration offload (0/1)");

/* nr_dma_chan: max DMA channels to use per batch */
static int nr_dma_chan_param_set(const char *val, const struct kernel_param *kp)
{
	unsigned int new_val;
	int ret;

	ret = kstrtouint(val, 0, &new_val);
	if (ret)
		return ret;
	if (new_val < 1 || new_val > MAX_DMA_CHANNELS)
		return -EINVAL;

	mutex_lock(&dcbm_mutex);
	WRITE_ONCE(nr_dma_channels, new_val);
	mutex_unlock(&dcbm_mutex);
	return 0;
}

static int nr_dma_chan_param_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%u\n", READ_ONCE(nr_dma_channels));
}

static const struct kernel_param_ops nr_dma_chan_param_ops = {
	.set = nr_dma_chan_param_set,
	.get = nr_dma_chan_param_get,
};
module_param_cb(nr_dma_chan, &nr_dma_chan_param_ops, NULL, 0644);
MODULE_PARM_DESC(nr_dma_chan,
		 "DMA channels to request when offloading is enabled (1..16)");

/* reason_mask: set of MR_* reasons this migrator handles */
static int reason_mask_param_set(const char *val, const struct kernel_param *kp)
{
	unsigned long mask;
	int ret;

	ret = migrate_offload_reason_mask_parse(val, &mask);
	if (ret)
		return ret;

	mutex_lock(&dcbm_mutex);
	WRITE_ONCE(dcbm_reason_mask, mask);
	if (offloading_enabled)
		migrate_offload_set_reason_mask(&dma_migrator, mask);
	mutex_unlock(&dcbm_mutex);
	return 0;
}

static int reason_mask_param_get(char *buffer, const struct kernel_param *kp)
{
	return migrate_offload_reason_mask_format(buffer, READ_ONCE(dcbm_reason_mask));
}

static const struct kernel_param_ops reason_mask_param_ops = {
	.set = reason_mask_param_set,
	.get = reason_mask_param_get,
};
module_param_cb(reason_mask, &reason_mask_param_ops, NULL, 0644);
MODULE_PARM_DESC(reason_mask,
		 "Reasons to offload: comma-separated names (e.g. compaction,demotion), 'all', 'none', or a raw hex mask");

/* folios_migrated / folios_failures: counters; any write resets to 0 */
static int folios_migrated_param_set(const char *val, const struct kernel_param *kp)
{
	atomic_long_set(&folios_migrated, 0);
	return 0;
}

static int folios_migrated_param_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%ld\n", atomic_long_read(&folios_migrated));
}

static const struct kernel_param_ops folios_migrated_param_ops = {
	.set = folios_migrated_param_set,
	.get = folios_migrated_param_get,
};
module_param_cb(folios_migrated, &folios_migrated_param_ops, NULL, 0644);
MODULE_PARM_DESC(folios_migrated, "Folios DMA-copied (write to reset)");

static int folios_failures_param_set(const char *val, const struct kernel_param *kp)
{
	atomic_long_set(&folios_failures, 0);
	return 0;
}

static int folios_failures_param_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%ld\n", atomic_long_read(&folios_failures));
}

static const struct kernel_param_ops folios_failures_param_ops = {
	.set = folios_failures_param_set,
	.get = folios_failures_param_get,
};
module_param_cb(folios_failures, &folios_failures_param_ops, NULL, 0644);
MODULE_PARM_DESC(folios_failures, "DMA-copy failure count (write to reset)");

static int __init dcbm_init(void)
{
	pr_info("dcbm: DMA Core Batch Migrator initialized\n");
	return 0;
}

static void __exit dcbm_exit(void)
{
	struct dcbm_dying *d, *tmp;
	unsigned int i;

	mutex_lock(&dcbm_mutex);
	if (offloading_enabled) {
		migrate_offload_unregister(&dma_migrator);
		dcbm_release_channels();
		WRITE_ONCE(offloading_enabled, false);
	}
	for (i = 0; i < dcbm_nr_buses; i++)
		bus_unregister_notifier(dcbm_buses[i], &dcbm_bus_nbs[i]);
	dcbm_nr_buses = 0;
	mutex_unlock(&dcbm_mutex);
	cancel_work_sync(&dcbm_fill_work);

	list_for_each_entry_safe(d, tmp, &dcbm_dying_devs, node) {
		list_del(&d->node);
		kfree(d);
	}

	pr_info("dcbm: DMA Core Batch Migrator unloaded\n");
}

module_init(dcbm_init);
module_exit(dcbm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shivank Garg");
MODULE_DESCRIPTION("DMA Core Batch Migrator");
