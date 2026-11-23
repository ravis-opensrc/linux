// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI dmaengine provider
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>

#include "../dmaengine.h"
#include "../virt-dma.h"
#include "completion.h"
#include "context.h"
#include "descriptor.h"
#include "dma.h"
#include "ring.h"
#include "sdxi.h"

static unsigned short dma_channels = 1;
module_param(dma_channels, ushort, 0644);
MODULE_PARM_DESC(dma_channels, "DMA channels per function (default: 1)");

/*
 * Each channel owns an SDXI context. Transactions enter the ring in cookie
 * order. Each copy produces one descriptor with its own completion block, and
 * batches end with a shared fenced interrupt descriptor. A memset produces
 * several descriptors, forms a batch of its own, and its last descriptor
 * is fenced and carries the completion block. Each transaction owns its
 * completion block until reaped. Transactions awaiting capacity remain
 * pending. Termination retains published transactions for reaping while
 * suppressing their callbacks.
 */

/*
 * Byte patterns for memset. A pattern page is shared by every in-flight
 * memset of that value and is reused once nothing refers to it.
 */
#define SDXI_NR_PATTERNS 8

struct sdxi_dma_pattern {
	unsigned int refs;
	u8 value;
};

struct sdxi_dma_chan {
	struct virt_dma_chan vchan;
	struct sdxi_cxt *cxt;
	unsigned int vector;
	unsigned int irq;
	struct sdxi_akey_ent *akey;
	/*
	 * Transactions awaiting ring slots or completion blocks; protected by
	 * vchan.lock.
	 */
	struct list_head pending;

	spinlock_t pattern_lock;	/* Protects pattern[] */
	struct sdxi_dma_pattern pattern[SDXI_NR_PATTERNS];
	u8 *pattern_pages;		/* SDXI_NR_PATTERNS pages of SZ_4K */
	dma_addr_t pattern_dma;
};

struct sdxi_dma_op {
	bool repeat;
	union {
		struct sdxi_copy copy;
		struct sdxi_repcopy repcopy;
	};
};

struct sdxi_dma_txd {
	struct virt_dma_desc vdesc;
	/* Completion ownership is acquired at issue and released after reaping. */
	struct sdxi_completion completion;
	int pattern;	/* Held pattern slot, or -1 */
	unsigned int nr_ops;
	struct sdxi_dma_op ops[] __counted_by(nr_ops);
};

static void sdxi_dma_reap_locked(struct sdxi_dma_chan *sdchan);

struct sdxi_dma_dev {
	struct dma_device dma_dev;
	size_t nr_channels;
	struct sdxi_dma_chan sdchan[] __counted_by(nr_channels);
};

static struct sdxi_dma_chan *to_sdxi_dma_chan(const struct dma_chan *dma_chan)
{
	const struct virt_dma_chan *vchan;

	vchan = container_of_const(dma_chan, struct virt_dma_chan, chan);
	return container_of(vchan, struct sdxi_dma_chan, vchan);
}

static struct sdxi_dma_txd *
to_sdxi_dma_txd(const struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct sdxi_dma_txd, vdesc);
}

static void sdxi_dma_txd_put(struct sdxi_dma_chan *sdchan,
			     struct sdxi_dma_txd *txd)
{
	if (txd->pattern >= 0) {
		scoped_guard(spinlock_irqsave, &sdchan->pattern_lock)
			sdchan->pattern[txd->pattern].refs--;
	}
	kfree(txd);
}

static void sdxi_dma_txd_free(struct virt_dma_desc *vdesc)
{
	sdxi_dma_txd_put(to_sdxi_dma_chan(vdesc->tx.chan),
			 to_sdxi_dma_txd(vdesc));
}

static struct sdxi_dma_txd *sdxi_dma_txd_alloc(unsigned int nr_ops)
{
	struct sdxi_dma_txd *txd;

	txd = kzalloc(struct_size(txd, ops, nr_ops), GFP_NOWAIT);
	if (!txd)
		return NULL;
	txd->nr_ops = nr_ops;
	txd->pattern = -1;
	return txd;
}

static struct dma_async_tx_descriptor *
sdxi_dma_prep_memcpy(struct dma_chan *dma_chan, dma_addr_t dst,
		     dma_addr_t src, size_t len, unsigned long flags)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);
	struct sdxi_cxt *cxt = sdchan->cxt;
	u16 akey_index = sdxi_akey_index(cxt, sdchan->akey);
	struct sdxi_dma_txd *txd;
	struct sdxi_copy copy = {
		.src = src,
		.dst = dst,
		.src_akey = akey_index,
		.dst_akey = akey_index,
		.len = len,
	};

	/*
	 * Validate now, with a throwaway encode onto the stack. The real
	 * encode into the ring happens at issue.
	 */
	if (sdxi_encode_copy(&(struct sdxi_desc){}, &copy))
		return NULL;

	txd = sdxi_dma_txd_alloc(1);
	if (!txd)
		return NULL;
	txd->ops[0].copy = copy;

	return vchan_tx_prep(&sdchan->vchan, &txd->vdesc, flags);
}

/* Take a pattern slot holding @value, filling a free one if needed. */
static int sdxi_dma_pattern_get(struct sdxi_dma_chan *sdchan, u8 value)
{
	int free = -1;

	guard(spinlock_irqsave)(&sdchan->pattern_lock);

	for (int i = 0; i < SDXI_NR_PATTERNS; i++) {
		struct sdxi_dma_pattern *pat = &sdchan->pattern[i];

		if (pat->refs && pat->value == value) {
			pat->refs++;
			return i;
		}
		if (!pat->refs && free < 0)
			free = i;
	}
	if (free < 0)
		return -EBUSY;

	memset(sdchan->pattern_pages + free * SZ_4K, value, SZ_4K);
	sdchan->pattern[free].value = value;
	sdchan->pattern[free].refs = 1;
	return free;
}

static void sdxi_dma_op_copy(struct sdxi_dma_op *op, dma_addr_t dst,
			     dma_addr_t src, u64 len, u16 akey)
{
	op->repeat = false;
	op->copy = (struct sdxi_copy) {
		.src = src,
		.dst = dst,
		.len = len,
		.src_akey = akey,
		.dst_akey = akey,
	};
}

static struct dma_async_tx_descriptor *
sdxi_dma_prep_memset(struct dma_chan *dma_chan, dma_addr_t dst, int value,
		     size_t len, unsigned long flags)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);
	struct sdxi_cxt *cxt = sdchan->cxt;
	u16 akey_index = sdxi_akey_index(cxt, sdchan->akey);
	struct sdxi_fill_plan plan;
	struct sdxi_dma_txd *txd;
	dma_addr_t pat_dma;
	unsigned int n = 0;
	int pat;

	if (sdxi_plan_fill(dst, len, cxt->sdxi->max_buffer, &plan))
		return NULL;
	/* The fill and its terminator must fit the ring in one batch. */
	if ((u64)plan.nr_body + !!plan.head + !!plan.tail >
	    cxt->ring_state->sq->count - 2)
		return NULL;

	pat = sdxi_dma_pattern_get(sdchan, (u8)value);
	/* More than SDXI_NR_PATTERNS distinct values in flight. */
	if (pat < 0)
		return NULL;
	pat_dma = sdchan->pattern_dma + pat * SZ_4K;

	txd = sdxi_dma_txd_alloc(!!plan.head + plan.nr_body + !!plan.tail);
	if (!txd) {
		scoped_guard(spinlock_irqsave, &sdchan->pattern_lock)
			sdchan->pattern[pat].refs--;
		return NULL;
	}
	txd->pattern = pat;

	/* Any offset into a uniform pattern page serves as the source. */
	if (plan.head)
		sdxi_dma_op_copy(&txd->ops[n++], dst, pat_dma, plan.head, akey_index);
	dst += plan.head;
	for (u64 left = plan.body; left;) {
		u64 this = min(left, plan.chunk);

		txd->ops[n].repeat = true;
		txd->ops[n].repcopy = (struct sdxi_repcopy) {
			.src = pat_dma,
			.dst = dst,
			.src_len = SZ_4K,
			.nr_copies = this / SZ_4K,
			.src_akey = akey_index,
			.dst_akey = akey_index,
			.zero = !value,
		};
		/* Validate now; the real encode happens at issue. */
		if (sdxi_encode_repcopy(&(struct sdxi_desc){}, &txd->ops[n].repcopy)) {
			sdxi_dma_txd_put(sdchan, txd);
			return NULL;
		}
		n++;
		dst += this;
		left -= this;
	}
	if (plan.tail)
		sdxi_dma_op_copy(&txd->ops[n++], dst, pat_dma, plan.tail, akey_index);

	for (unsigned int i = 0; i < n; i++) {
		if (!txd->ops[i].repeat &&
		    sdxi_encode_copy(&(struct sdxi_desc){}, &txd->ops[i].copy)) {
			sdxi_dma_txd_put(sdchan, txd);
			return NULL;
		}
	}

	return vchan_tx_prep(&sdchan->vchan, &txd->vdesc, flags);
}

static enum dma_status sdxi_tx_status(struct dma_chan *chan,
				      dma_cookie_t cookie,
				      struct dma_tx_state *state)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(chan);
	struct virt_dma_desc *vdesc;

	/* Under the lock for a consistent cookie snapshot; the IRQ reaps. */
	guard(spinlock_irqsave)(&sdchan->vchan.lock);

	/* vchan_find_desc() only returns issued txds, which own a completion. */
	vdesc = vchan_find_desc(&sdchan->vchan, cookie);
	if (vdesc) {
		struct sdxi_dma_txd *txd = to_sdxi_dma_txd(vdesc);

		if (sdxi_cst_blk_check(txd->completion.cst_blk) == SDXI_COMPLETION_ERROR)
			return DMA_ERROR;
	}

	return dma_cookie_status(chan, cookie, state);
}

/*
 * Fill capacity with complete, fenced batches in cookie order. Return true
 * if descriptors were published; the caller kicks once with the lock held.
 */
static bool sdxi_dma_drain_locked(struct sdxi_dma_chan *sdchan)
{
	struct sdxi_cxt *cxt = sdchan->cxt;
	struct sdxi_ring_state *rs = cxt->ring_state;
	bool pushed = false;

	lockdep_assert_held(&sdchan->vchan.lock);

	while (!list_empty(&sdchan->pending)) {
		struct virt_dma_desc *vdesc;
		struct sdxi_ring_resv resv;
		struct sdxi_desc *desc;
		size_t claimed = 0, copies, i = 0;
		bool multi = false;
		size_t nr_slots;

		/*
		 * Claim a pending prefix, stopping at a flag boundary or capacity
		 * limit. A transaction with several descriptors is a batch of its own.
		 */
		list_for_each_entry(vdesc, &sdchan->pending, node) {
			struct sdxi_dma_txd *txd = to_sdxi_dma_txd(vdesc);

			if (txd->nr_ops > 1) {
				if (claimed)
					break;
				multi = true;
			}
			if (!sdxi_cst_pool_get(cxt->cst_pool, &txd->completion))
				break;
			claimed++;
			if (multi ||
			    vdesc->tx.flags & (DMA_PREP_INTERRUPT | DMA_PREP_FENCE) ||
			    claimed == rs->sq->count - 1)
				break;
		}
		if (!claimed)
			break;

		/* Ring reservations cannot be undone; acquire completions first. */
		copies = 0;
		if (multi) {
			vdesc = list_first_entry(&sdchan->pending,
						 struct virt_dma_desc, node);
			if (!sdxi_ring_try_reserve(rs, to_sdxi_dma_txd(vdesc)->nr_ops + 1,
						   &resv))
				copies = 1;
		} else if (!sdxi_ring_try_reserve_range(rs, 2, claimed + 1, &resv)) {
			copies = sdxi_ring_resv_count(&resv) - 1;
		}

		/* Return claims not covered by this (possibly failed) reservation. */
		list_for_each_entry(vdesc, &sdchan->pending, node) {
			struct sdxi_dma_txd *txd = to_sdxi_dma_txd(vdesc);

			if (i == claimed)
				break;
			if (i++ < copies)
				continue;
			sdxi_cst_pool_put(cxt->cst_pool, txd->completion.cst_blk);
			txd->completion = (struct sdxi_completion) {};
		}
		if (!copies)
			break;
		nr_slots = sdxi_ring_resv_count(&resv);
		for (size_t c = 0, s = 0; c < copies; c++) {
			struct sdxi_dma_txd *txd;

			vdesc = list_first_entry(&sdchan->pending,
						 struct virt_dma_desc, node);
			txd = to_sdxi_dma_txd(vdesc);
			for (unsigned int k = 0; k < txd->nr_ops; k++, s++) {
				const struct sdxi_dma_op *op = &txd->ops[k];
				struct sdxi_slot slot = sdxi_ring_resv_slot(&resv, s);

				desc = sdxi_slot_desc(slot);
				/* validated at prep */
				if (op->repeat)
					(void)sdxi_encode_repcopy(desc, &op->repcopy);
				else
					(void)sdxi_encode_copy(desc, &op->copy);
				if (k == txd->nr_ops - 1) {
					/* Complete only after the earlier descriptors. */
					if (k)
						sdxi_desc_set_fence(desc);
					sdxi_cxt_attach_cst(slot, &txd->completion);
				}
				sdxi_desc_make_valid(desc);
			}
			list_move_tail(&vdesc->node, &sdchan->vchan.desc_issued);
		}

		/*
		 * FE waits for all copies before notifying and before parsing
		 * subsequent copies (SDXI 1.0a 5.3, steps 7-9). One terminator
		 * satisfies both flags and supplies internal completion IRQs.
		 */
		desc = sdxi_slot_desc(sdxi_ring_resv_slot(&resv, nr_slots - 1));
		sdxi_encode_intr(desc, &(const struct sdxi_intr) {
			.akey = sdxi_akey_index(cxt, sdchan->akey),
		});
		sdxi_desc_set_fence(desc);
		sdxi_desc_make_valid(desc);
		pushed = true;
	}

	return pushed;
}

static void sdxi_dma_issue_pending(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);

	guard(spinlock_irqsave)(&sdchan->vchan.lock);

	list_splice_tail_init(&sdchan->vchan.desc_submitted, &sdchan->pending);
	if (sdxi_dma_drain_locked(sdchan))
		sdxi_cxt_kick(sdchan->cxt);
}

static int sdxi_dma_terminate_all(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);
	struct virt_dma_chan *vchan = &sdchan->vchan;
	LIST_HEAD(head);

	/*
	 * Prepared descriptors remain caller-owned. Free submitted and pending
	 * transactions, and move published transactions to desc_terminated in ring
	 * order for callback-free reaping.
	 */
	scoped_guard(spinlock_irqsave, &vchan->lock) {
		list_splice_tail_init(&sdchan->pending, &head);
		list_splice_tail_init(&vchan->desc_submitted, &head);
		list_splice_tail_init(&vchan->desc_issued, &vchan->desc_terminated);
	}
	vchan_dma_desc_free_list(vchan, &head);

	return 0;
}

/* Bound the wait for issued and pending copies to complete. */
#define SDXI_DMA_SYNC_TIMEOUT_US	(10 * USEC_PER_SEC)

/*
 * Reap without relying on the completion IRQ, and report whether the
 * device is done with every txd that has been or may be placed in the
 * ring. Pending txds count because dma_chan_put() synchronizes without
 * terminating first.
 */
static bool sdxi_dma_idle(struct sdxi_dma_chan *sdchan)
{
	struct virt_dma_chan *vchan = &sdchan->vchan;

	guard(spinlock_irqsave)(&vchan->lock);

	sdxi_dma_reap_locked(sdchan);
	if (sdxi_dma_drain_locked(sdchan))
		sdxi_cxt_kick(sdchan->cxt);

	return list_empty(&vchan->desc_terminated) &&
	       list_empty(&vchan->desc_issued) &&
	       list_empty(&sdchan->pending);
}

static void sdxi_dma_synchronize(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);
	struct virt_dma_chan *vchan = &sdchan->vchan;
	LIST_HEAD(head);
	bool idle;
	int err;

	/*
	 * Callers cannot handle failure, so the wait is not interruptible.
	 * Every completion was reaped under vchan.lock, so the tasklet that
	 * runs its callback is already scheduled for vchan_synchronize().
	 */
	err = read_poll_timeout(sdxi_dma_idle, idle, idle, 100,
				SDXI_DMA_SYNC_TIMEOUT_US, false, sdchan);
	if (err) {
		/*
		 * Suppress callbacks for outstanding transactions. Completion blocks
		 * not released by the reaper stay claimed until context teardown.
		 */
		scoped_guard(spinlock_irqsave, &vchan->lock) {
			list_splice_tail_init(&vchan->desc_issued,
					      &vchan->desc_terminated);
			list_splice_tail_init(&sdchan->pending, &head);
		}
		vchan_dma_desc_free_list(vchan, &head);
		dev_err(dma_chan->device->dev,
			"timed out waiting for DMA channel %d to drain\n",
			dma_chan->chan_id);
	}

	vchan_synchronize(vchan);
}

static void sdxi_dma_reap_locked(struct sdxi_dma_chan *sdchan)
{
	struct virt_dma_chan *vchan = &sdchan->vchan;
	struct virt_dma_desc *vdesc, *tmp;

	lockdep_assert_held(&vchan->lock);

	/*
	 * Terminated txds are still in the ring. Release them once the device
	 * is done, without completing their cookies or invoking callbacks.
	 */
	list_for_each_entry_safe(vdesc, tmp, &vchan->desc_terminated, node) {
		struct sdxi_dma_txd *txd = to_sdxi_dma_txd(vdesc);

		if (sdxi_cst_blk_check(txd->completion.cst_blk) == SDXI_COMPLETION_PENDING)
			break;

		list_del(&vdesc->node);
		sdxi_cst_pool_put(sdchan->cxt->cst_pool, txd->completion.cst_blk);
		sdxi_dma_txd_free(vdesc);
	}

	/* Reap completed transactions in cookie order. */
	while ((vdesc = vchan_next_desc(vchan))) {
		struct sdxi_dma_txd *txd = to_sdxi_dma_txd(vdesc);
		enum sdxi_completion_state state;

		state = sdxi_cst_blk_check(txd->completion.cst_blk);
		if (state == SDXI_COMPLETION_PENDING)
			break;

		if (state == SDXI_COMPLETION_ERROR)
			txd->vdesc.tx_result.result = DMA_TRANS_ABORTED;

		list_del(&vdesc->node);
		sdxi_cst_pool_put(sdchan->cxt->cst_pool, txd->completion.cst_blk);
		txd->completion = (struct sdxi_completion) {};
		vchan_cookie_complete(&txd->vdesc);
	}

	sdxi_ring_wake_up(sdchan->cxt->ring_state);
}

static irqreturn_t sdxi_dma_cxt_irq(int irq, void *data)
{
	struct sdxi_dma_chan *sdchan = data;

	guard(spinlock_irqsave)(&sdchan->vchan.lock);

	sdxi_dma_reap_locked(sdchan);
	/* Consumed completion blocks may let stalled pending txds proceed. */
	if (sdxi_dma_drain_locked(sdchan))
		sdxi_cxt_kick(sdchan->cxt);

	return IRQ_HANDLED;
}

static int sdxi_dma_alloc_chan_resources(struct dma_chan *dma_chan)
{
	struct sdxi_dev *sdxi = dev_get_drvdata(dma_chan->device->dev);
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);
	int vector, irq, err;

	sdchan->pattern_pages = dma_alloc_coherent(sdxi->dev,
						   SDXI_NR_PATTERNS * SZ_4K,
						   &sdchan->pattern_dma, GFP_KERNEL);
	if (!sdchan->pattern_pages)
		return -ENOMEM;
	memset(sdchan->pattern, 0, sizeof(sdchan->pattern));

	sdchan->cxt = sdxi_cxt_new(sdxi);
	if (!sdchan->cxt) {
		err = -ENOMEM;
		goto free_pattern;
	}
	err = vector = sdxi_alloc_vector(sdxi);
	if (vector < 0)
		goto exit_cxt;

	sdchan->vector = vector;

	err = irq = sdxi_vector_to_irq(sdxi, vector);
	if (irq < 0)
		goto free_vector;

	sdchan->irq = irq;

	/*
	 * Note this akey entry is used for both the completion
	 * interrupt and source and destination access for copies.
	 */
	sdchan->akey = sdxi_alloc_akey(sdchan->cxt);
	if (!sdchan->akey) {
		err = -ENOMEM;
		goto free_vector;
	}

	*sdchan->akey = (typeof(*sdchan->akey)) {
		.intr_num = cpu_to_le16(FIELD_PREP(SDXI_AKEY_ENT_VL, 1) |
					FIELD_PREP(SDXI_AKEY_ENT_IV, 1) |
					FIELD_PREP(SDXI_AKEY_ENT_INTR_NUM,
						   vector)),
	};

	err = request_irq(sdchan->irq, sdxi_dma_cxt_irq,
			  IRQF_TRIGGER_NONE, "SDXI DMAengine", sdchan);
	if (err)
		goto free_akey;

	err = sdxi_start_cxt(sdchan->cxt);
	if (err)
		goto free_irq;

	return 0;
free_irq:
	free_irq(sdchan->irq, sdchan);
free_akey:
	sdxi_free_akey(sdchan->cxt, sdchan->akey);
free_vector:
	sdxi_free_vector(sdxi, vector);
exit_cxt:
	sdxi_cxt_exit(sdchan->cxt);
free_pattern:
	dma_free_coherent(sdxi->dev, SDXI_NR_PATTERNS * SZ_4K,
			  sdchan->pattern_pages, sdchan->pattern_dma);
	return err;
}

static void sdxi_dma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);
	LIST_HEAD(head);

	sdxi_stop_cxt(sdchan->cxt);
	free_irq(sdchan->irq, sdchan);
	sdxi_free_vector(sdchan->cxt->sdxi, sdchan->vector);
	sdxi_free_akey(sdchan->cxt, sdchan->akey);
	/* virt-dma does not know about the driver's pending list. */
	scoped_guard(spinlock_irqsave, &sdchan->vchan.lock)
		list_splice_tail_init(&sdchan->pending, &head);
	vchan_dma_desc_free_list(&sdchan->vchan, &head);
	vchan_free_chan_resources(to_virt_chan(dma_chan));
	sdxi_cxt_exit(sdchan->cxt);
	dma_free_coherent(dma_chan->device->dev, SDXI_NR_PATTERNS * SZ_4K,
			  sdchan->pattern_pages, sdchan->pattern_dma);
}

int sdxi_dma_register(struct sdxi_dev *sdxi)
{
	struct device *dev = sdxi->dev;
	struct sdxi_dma_dev *sddev;
	struct dma_device *dma_dev;
	unsigned int nr_channels = dma_channels; /* writable module param; snapshot it */
	int err;

	if (!nr_channels)
		return 0;
	/*
	 * TODO: Require IntrGrp support before registering the DMAengine provider
	 * (SDXI 1.0a Table 6-1).
	 */

	sddev = devm_kzalloc(dev, struct_size(sddev, sdchan, nr_channels),
			     GFP_KERNEL);
	if (!sddev)
		return -ENOMEM;

	sddev->nr_channels = nr_channels;

	dma_dev = &sddev->dma_dev;
	*dma_dev = (typeof(*dma_dev)) {
		.dev                 = dev,
		.src_addr_widths     = DMA_SLAVE_BUSWIDTH_64_BYTES,
		.dst_addr_widths     = DMA_SLAVE_BUSWIDTH_64_BYTES,
		.directions          = BIT(DMA_MEM_TO_MEM),
		.residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR,

		.device_alloc_chan_resources = sdxi_dma_alloc_chan_resources,
		.device_free_chan_resources  = sdxi_dma_free_chan_resources,

		.device_prep_dma_memcpy = sdxi_dma_prep_memcpy,
		.device_prep_dma_memset = sdxi_dma_prep_memset,
		.fill_align = DMAENGINE_ALIGN_1_BYTE,

		.device_terminate_all = sdxi_dma_terminate_all,
		.device_synchronize = sdxi_dma_synchronize,
		.device_tx_status = sdxi_tx_status,
		.device_issue_pending = sdxi_dma_issue_pending,
	};

	dma_cap_set(DMA_MEMCPY, dma_dev->cap_mask);
	dma_cap_set(DMA_MEMSET, dma_dev->cap_mask);
	INIT_LIST_HEAD(&dma_dev->channels);

	for (size_t i = 0; i < sddev->nr_channels; ++i) {
		struct sdxi_dma_chan *sdchan = &sddev->sdchan[i];

		sdchan->vchan.desc_free = sdxi_dma_txd_free;
		vchan_init(&sdchan->vchan, &sddev->dma_dev);
		INIT_LIST_HEAD(&sdchan->pending);
		spin_lock_init(&sdchan->pattern_lock);
	}

	err = dmaenginem_async_device_register(dma_dev);
	if (err)
		return dev_warn_probe(dev, err, "failed to register dma device\n");

	return 0;
}
