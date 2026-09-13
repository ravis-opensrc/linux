/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON perf-event source - PMU-agnostic overflow handling
 *
 * Turns PMU overflow samples into DAMON access reports.  The overflow
 * handler runs in NMI context and reports through damon_report_access(),
 * so it works with either paddr or vaddr ops.
 *
 * Only the address fields the PMU provides are set: paddr when
 * PERF_SAMPLE_PHYS_ADDR is set, vaddr when PERF_SAMPLE_ADDR is set, plus
 * the CPU the handler runs on.  The unified drain matches on whichever is
 * present.  Both the thread id and the thread group id are reported,
 * because a VA-only PMU samples whichever thread of a process happened to
 * access memory while the drain matches a target created for the process.
 */

#include <linux/cpuhotplug.h>
#include <linux/damon.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include "perf_source.h"

/* PMU event attribute for perf-event probe configuration */
struct damon_perf_event_attr {
	u32 type;
	u64 config;
	u64 config1;
	u64 config2;
	bool sample_phys_addr;
	bool sample_weight_struct;
	bool exclude_kernel;
	bool exclude_hv;
	bool freq;
	u64 sample_freq;
	u64 sample_period;
	u32 wakeup_events;
	u32 precise_ip;
};

struct damon_perf_probe_event {
	struct damon_perf_event_attr attr;
	struct damon_ctx *ctx;	/* owning ctx for ring routing; set at setup */
	void *priv;		/* struct damon_perf_probe_state * */
	struct hlist_node hlist_node;
	int probe_idx;		/* 1-based index into probe_hits[]; set at setup */
};

struct damon_perf_probe_state {
	struct perf_event * __percpu *event;
};

static void damon_perf_overflow(struct perf_event *perf_event,
				struct perf_sample_data *data,
				struct pt_regs *regs)
{
	struct damon_perf_probe_event *event =
		(struct damon_perf_probe_event *)perf_event->overflow_handler_context;
	struct damon_access_report report = {
		.size = PAGE_SIZE,	/* one sample reports one access point */
		.cpu = smp_processor_id(),
	};
	struct damon_ctx *ctx;
	int probe_idx;

	/*
	 * Teardown NULLs event->ctx (with a release barrier) before releasing
	 * the per-CPU perf events.  An NMI overflow that has already passed
	 * the check below may still be executing; it runs to completion and
	 * enqueues to the per-CPU ring, which outlives the event (freed after
	 * kdamond exits), so no use-after-free occurs.  A new NMI starting
	 * after the store sees NULL and returns immediately.  Pairs with the
	 * smp_store_release() in damon_perf_probe_teardown().
	 *
	 * Ordering note: an NMI that observes a non-NULL ctx reads
	 * event->probe_idx within nanoseconds of the check, while teardown's
	 * kfree() of the event follows the release store by microseconds, so
	 * the probe_idx read cannot race the free.
	 */
	ctx = smp_load_acquire(&event->ctx);
	if (!ctx)
		return;
	probe_idx = event->probe_idx;

	/* probe_idx 0 is the zero-init sentinel; a valid index must be >= 1 */
	if (WARN_ONCE(probe_idx == 0,
		      "damon-perf: overflow handler called with probe_idx=0\n"))
		return;

	report.probe_idx = probe_idx;
	report.ctx = ctx;	/* route to this ctx's per-ctx perf ring */

	if (!data)
		return;

	/*
	 * Populate whichever address fields the PMU provides.
	 * IBS provides PA (and optionally VA); PEBS provides VA only.
	 * Gate on sample_flags rather than testing for zero: the flag is
	 * the authoritative indicator of field validity.
	 */
	if (data->sample_flags & PERF_SAMPLE_PHYS_ADDR)
		report.paddr = data->phys_addr & PAGE_MASK;
	if (data->sample_flags & PERF_SAMPLE_ADDR)
		report.vaddr = data->addr & PAGE_MASK;

	if (!report.paddr && !report.vaddr)
		return;

	/*
	 * data_src is only valid when the PMU recorded it; otherwise
	 * report.is_write stays false (zero-initialised above).
	 */
	if (data->sample_flags & PERF_SAMPLE_DATA_SRC)
		report.is_write = !!(data->data_src.mem_op & PERF_MEM_OP_STORE);
	report.tid = task_pid_vnr(current);
	report.tgid = task_tgid_vnr(current);

	/*
	 * Best-effort: a false return means the sample was dropped (ring
	 * full, teardown race, or NMI-nested producer).  Sampling is lossy
	 * by nature; there is nothing useful the NMI handler can do about a
	 * drop.
	 */
	damon_report_access(&report);
}

static enum cpuhp_state damon_perf_cpuhp_state;

/*
 * Per-PMU exclusivity: each PMU type may be owned by at most one damon_ctx.
 * Multiple probes from the same ctx sharing a PMU type are allowed; a second
 * ctx attempting to grab a PMU type already owned returns -EBUSY.
 *
 * The table is deliberately keyed on the coarse perf_event_attr.type (the
 * PERF_TYPE_* value) rather than the physical PMU.  This exclusivity is a
 * DAMON-level policy to keep hardware-counter attribution unambiguous, and
 * a DAMON context drives one sampling facility at a time.  Distinct physical
 * PMUs that happen to share a type value (e.g. PERF_TYPE_RAW on a core PMU
 * versus an uncore PMU) therefore contend for the same slot; that coarse
 * granularity is the intended contract for this source.
 */
struct damon_pmu_owner {
	struct list_head node;
	u32 pmu_type;
	atomic_long_t owner_ctx;
	atomic_t refcount;
};

static LIST_HEAD(damon_pmu_owner_list);
static DEFINE_SPINLOCK(damon_pmu_owner_lock);

static void damon_perf_event_init_attr(struct damon_perf_probe_event *event,
				       struct perf_event_attr *attr)
{
	u64 stype = PERF_SAMPLE_TIME | PERF_SAMPLE_PERIOD | PERF_SAMPLE_ADDR;

	/*
	 * Gate PERF_SAMPLE_PHYS_ADDR on the probe attribute: Intel PEBS
	 * has sample_phys_addr=0 by default; forcing it triggers an
	 * NMI-context pagetable walk per sample and sets entry->paddr
	 * unconditionally, which prevents the vaddr drain branch from
	 * ever being exercised.
	 */
	if (event->attr.sample_phys_addr)
		stype |= PERF_SAMPLE_PHYS_ADDR;
	if (event->attr.sample_weight_struct)
		stype |= PERF_SAMPLE_WEIGHT_STRUCT;
	stype |= PERF_SAMPLE_DATA_SRC;

	*attr = (struct perf_event_attr) {
		.size = sizeof(*attr),
		.type = event->attr.type,
		.config = event->attr.config,
		.config1 = event->attr.config1,
		.config2 = event->attr.config2,
		.freq = event->attr.freq,
		.sample_type = stype,
		.precise_ip = event->attr.precise_ip,
		.pinned = 1,
		/*
		 * Created disabled, and enabled by the caller once the counter
		 * is fully set up.
		 */
		.disabled = 1,
		.wakeup_events = event->attr.wakeup_events,
		.exclude_kernel = event->attr.exclude_kernel,
		.exclude_hv = event->attr.exclude_hv,
	};
	if (event->attr.freq)
		attr->sample_freq = event->attr.sample_freq;
	else
		attr->sample_period = event->attr.sample_period;
}

static int damon_perf_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct damon_perf_probe_event *event = hlist_entry(node,
			struct damon_perf_probe_event, hlist_node);
	struct damon_perf_probe_state *perf = event->priv;
	struct perf_event_attr attr;
	struct perf_event *perf_event;

	if (!perf)
		return 0;

	damon_perf_event_init_attr(event, &attr);

	/*
	 * Pass @event (not a probe_idx cookie) as the overflow context:
	 * damon_perf_overflow() reads event->ctx via smp_load_acquire() for
	 * the teardown race barrier.
	 */
	perf_event = perf_event_create_kernel_counter(&attr, cpu, NULL,
						      damon_perf_overflow,
						      event);
	if (IS_ERR(perf_event))
		return PTR_ERR(perf_event);
	per_cpu(*perf->event, cpu) = perf_event;

	perf_event_enable(perf_event);
	return 0;
}

static int damon_perf_cpu_offline(unsigned int cpu, struct hlist_node *node)
{
	struct damon_perf_probe_event *event = hlist_entry(node,
			struct damon_perf_probe_event, hlist_node);
	struct damon_perf_probe_state *perf = event->priv;
	struct perf_event *perf_event;

	if (!perf)
		return 0;

	perf_event = per_cpu(*perf->event, cpu);
	if (perf_event) {
		perf_event_disable(perf_event);
		perf_event_release_kernel(perf_event);
		per_cpu(*perf->event, cpu) = NULL;
	}
	return 0;
}

/**
 * damon_perf_probe_setup - arm perf_events for a DAMON probe.
 * @ctx:   DAMON context that owns the probe.
 * @probe: the damon_probe being armed; it must be on ctx->probes, and its
 *         list position determines the probe_idx stored in ring entries.
 * @event: perf event descriptor (caller fills .attr fields).  On success,
 *         the descriptor is owned by the probe (released by
 *         damon_perf_probe_teardown()); on failure the caller retains it.
 *
 * Computes probe_idx by walking ctx->probes so the caller does not need
 * to track it externally.  probe_idx is baked from the probe's list
 * position at setup time; the list order must not change while the probe
 * is armed (the commit path re-syncs indices after any list mutation).
 * Returns 0 on success, negative errno on failure.
 */
int damon_perf_probe_setup(struct damon_ctx *ctx,
			   struct damon_probe *probe,
			   struct damon_perf_probe_event *event)
{
	struct damon_perf_probe_state *perf;
	struct damon_pmu_owner *owner, *found = NULL;
	struct damon_probe *p;
	bool probe_found = false;
	int idx = 0;
	int err = -ENOMEM;

	/*
	 * Per-PMU exclusivity: find or create an owner slot for this PMU type.
	 * Multiple probes from the same ctx sharing a PMU type are allowed;
	 * a second ctx attempting the same PMU type returns -EBUSY.
	 */
	spin_lock(&damon_pmu_owner_lock);
	list_for_each_entry(owner, &damon_pmu_owner_list, node) {
		if (owner->pmu_type == event->attr.type) {
			long cur = atomic_long_read(&owner->owner_ctx);

			if (cur != 0L && cur != (long)ctx) {
				spin_unlock(&damon_pmu_owner_lock);
				return -EBUSY;
			}
			atomic_long_set(&owner->owner_ctx, (long)ctx);
			atomic_inc(&owner->refcount);
			found = owner;
			break;
		}
	}
	if (!found) {
		/*
		 * GFP_ATOMIC: this allocation runs while holding
		 * damon_pmu_owner_lock (a spinlock), so it must not sleep.
		 */
		owner = kzalloc_obj(*owner, GFP_ATOMIC);
		if (!owner) {
			spin_unlock(&damon_pmu_owner_lock);
			return -ENOMEM;
		}
		owner->pmu_type = event->attr.type;
		atomic_long_set(&owner->owner_ctx, (long)ctx);
		atomic_set(&owner->refcount, 1);
		list_add(&owner->node, &damon_pmu_owner_list);
		found = owner;
	}
	spin_unlock(&damon_pmu_owner_lock);

	/* Compute probe_idx by walking ctx->probes list */
	damon_for_each_probe(p, ctx) {
		if (p == probe) {
			probe_found = true;
			break;
		}
		idx++;
	}
	/*
	 * A probe that is not on ctx->probes would silently get a bogus
	 * index (the list length); fail loudly instead.
	 */
	if (WARN_ON_ONCE(!probe_found)) {
		err = -EINVAL;
		goto release_owner;
	}
	/*
	 * probe_idx is 1-based (0 is the zero-init sentinel
	 * DAMON_PROBE_IDX_NONE); probe_hits[] is 0-based with
	 * DAMON_MAX_PROBES slots (indices 0..DAMON_MAX_PROBES-1).  idx is the
	 * 0-based list position, so the valid range is 0..DAMON_MAX_PROBES-1.
	 */
	if (idx >= DAMON_MAX_PROBES) {
		err = -ENOSPC;
		goto release_owner;
	}
	event->probe_idx = idx + 1;	/* 1-based; 0 is reserved sentinel */
	event->ctx = ctx;		/* route overflow reports to this ctx */

	/*
	 * Allocate the ctx's per-ctx perf report ring before arming any event,
	 * so the overflow handler always finds a ready ring.  Idempotent
	 * across a ctx's multiple probes.
	 */
	err = damon_ctx_alloc_perf_ring(ctx);
	if (err)
		goto release_owner;

	perf = kzalloc_obj(*perf, GFP_KERNEL);
	if (!perf)
		goto release_owner;
	event->priv = perf;

	perf->event = alloc_percpu(typeof(*perf->event));
	if (!perf->event)
		goto free_perf;

	INIT_HLIST_NODE(&event->hlist_node);

	err = cpuhp_state_add_instance(damon_perf_cpuhp_state,
				       &event->hlist_node);
	if (err)
		goto free_event;

	return 0;

free_event:
	free_percpu(perf->event);
free_perf:
	kfree(perf);
	event->priv = NULL;
release_owner:
	spin_lock(&damon_pmu_owner_lock);
	if (atomic_dec_and_test(&found->refcount)) {
		list_del(&found->node);
		spin_unlock(&damon_pmu_owner_lock);
		kfree(found);
	} else {
		spin_unlock(&damon_pmu_owner_lock);
	}
	return err;
}
EXPORT_SYMBOL_GPL(damon_perf_probe_setup);

/**
 * damon_perf_probe_teardown - disarm perf_events.
 * @ctx:   DAMON context that owns the probe (used to release per-PMU
 *         ownership).
 * @event: perf event descriptor previously passed to damon_perf_probe_setup().
 *
 * Teardown owns the event descriptor and frees it.  Must be called in
 * process context (a provider's event release may sleep).
 */
void damon_perf_probe_teardown(struct damon_ctx *ctx,
			       struct damon_perf_probe_event *event)
{
	struct damon_perf_probe_state *perf = event->priv;
	struct damon_pmu_owner *owner, *tmp;

	if (!perf)
		return;

	/*
	 * Signal in-flight NMI overflow handlers to drop samples before
	 * tearing down the perf events and freeing their backing state.
	 * Pairs with smp_load_acquire(&event->ctx) in damon_perf_overflow().
	 */
	smp_store_release(&event->ctx, NULL);

	/*
	 * cpuhp_state_remove_instance() disables and releases each CPU's perf
	 * event; once it returns, no new overflow can be delivered for this
	 * event.
	 */
	cpuhp_state_remove_instance(damon_perf_cpuhp_state,
				    &event->hlist_node);
	free_percpu(perf->event);
	kfree(perf);
	event->priv = NULL;

	/*
	 * Release per-PMU ownership when the last probe for this
	 * ctx/PMU-type pair is torn down.
	 */
	spin_lock(&damon_pmu_owner_lock);
	list_for_each_entry_safe(owner, tmp, &damon_pmu_owner_list, node) {
		if (owner->pmu_type == event->attr.type &&
		    atomic_long_read(&owner->owner_ctx) == (long)ctx) {
			/*
			 * Free under the lock so a concurrent same-PMU teardown
			 * cannot observe and free the same owner. kfree() under
			 * a non-irq spinlock in process context is safe.
			 */
			if (atomic_dec_and_test(&owner->refcount)) {
				list_del(&owner->node);
				kfree(owner);
			}
			break;
		}
	}
	spin_unlock(&damon_pmu_owner_lock);

	/* teardown owns the event allocation */
	kfree(event);
}
EXPORT_SYMBOL_GPL(damon_perf_probe_teardown);

static int __init damon_perf_source_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
				      "mm/damon/perf_source:online",
				      damon_perf_cpu_online,
				      damon_perf_cpu_offline);
	if (ret < 0)
		return ret;
	damon_perf_cpuhp_state = ret;
	return 0;
}

static void __exit damon_perf_source_exit(void)
{
	spin_lock(&damon_pmu_owner_lock);
	if (!list_empty(&damon_pmu_owner_list)) {
		spin_unlock(&damon_pmu_owner_lock);
		WARN(1, "damon_perf_source: unloading with active probes\n");
		return;
	}
	spin_unlock(&damon_pmu_owner_lock);
	cpuhp_remove_multi_state(damon_perf_cpuhp_state);
}

module_init(damon_perf_source_init);
module_exit(damon_perf_source_exit);
