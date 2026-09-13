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

/* struct damon_perf_probe_event is defined in perf_source.h. */

struct damon_perf_probe_state {
	struct perf_event * __percpu *event;	/* per-CPU probes (PEBS/IBS) */
	struct perf_event *single_event;	/* single-instance (system-wide PMU) */
	/*
	 * NMI-handler sample accounting: accepted counts samples queued via
	 * damon_report_access(), dropped counts samples it refused (ring
	 * full or teardown race).  atomic64 so the NMI handler can bump
	 * them without locking.
	 */
	atomic64_t accepted;
	atomic64_t dropped;
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
	struct damon_perf_probe_state *perf;
	int probe_idx;

	/*
	 * Teardown NULLs event->ctx (with a release barrier) before
	 * disabling/releasing the perf events.  A new NMI starting after the
	 * store sees NULL and returns immediately.  An NMI that already
	 * passed the ctx check takes a refcount reference (inc_not_zero);
	 * teardown drops the "armed" reference and spins until the count
	 * reaches zero before freeing the descriptor.  This closes the
	 * cross-CPU race: an NMI on another CPU inside the handler cannot
	 * have the descriptor freed under it.  The per-CPU ring the handler
	 * enqueues to outlives the event (freed only after kdamond exits),
	 * so the completed handler's damon_report_access() is safe.  Pairs
	 * with the smp_store_release() in damon_perf_probe_teardown().
	 *
	 * probe_idx is written once at setup (before any counter is armed)
	 * and never mutated afterwards, so the READ_ONCE here cannot observe
	 * a torn-down value.
	 *
	 * The refcount_inc_not_zero() pairs with teardown's dec-and-wait:
	 * if teardown has already dropped the armed reference, the increment
	 * fails and we return without touching the descriptor.  Otherwise we
	 * hold a reference for the handler's duration, so teardown cannot
	 * free the descriptor under us (even cross-CPU via NMI).
	 */
	ctx = smp_load_acquire(&event->ctx);
	if (!ctx)
		return;
	if (!refcount_inc_not_zero(&event->refcount))
		return;
	probe_idx = READ_ONCE(event->probe_idx);

	/* probe_idx 0 is the zero-init sentinel; a valid index must be >= 1 */
	if (WARN_ONCE(probe_idx == 0,
		      "damon-perf: overflow handler called with probe_idx=0\n")) {
		refcount_dec(&event->refcount);
		return;
	}

	report.probe_idx = probe_idx;
	report.ctx = ctx;	/* route to this ctx's per-ctx perf ring */

	if (!data) {
		refcount_dec(&event->refcount);
		return;
	}

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

	if (!report.paddr && !report.vaddr) {
		refcount_dec(&event->refcount);
		return;
	}

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
	 * drop beyond counting it.  event->priv stays valid for the
	 * handler's duration (see the lifetime note above).
	 */
	perf = READ_ONCE(event->priv);
	if (damon_report_access(&report)) {
		if (perf)
			atomic64_inc(&perf->accepted);
	} else {
		if (perf)
			atomic64_inc(&perf->dropped);
	}
	refcount_dec(&event->refcount);
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

/*
 * Take an extra reference on the owner for (@ctx, @type).  The owner must
 * exist (the caller holds a reference via an armed event).  Used to keep the
 * owner alive across a teardown/setup re-arm pair so a concurrent turn-on
 * cannot steal the PMU in between.  Returns true when the owner was found.
 */
static bool damon_pmu_owner_get(struct damon_ctx *ctx, u32 type)
{
	struct damon_pmu_owner *owner;
	bool found = false;

	spin_lock(&damon_pmu_owner_lock);
	list_for_each_entry(owner, &damon_pmu_owner_list, node) {
		if (owner->pmu_type == type &&
		    atomic_long_read(&owner->owner_ctx) == (long)ctx) {
			atomic_inc(&owner->refcount);
			found = true;
			break;
		}
	}
	spin_unlock(&damon_pmu_owner_lock);
	return found;
}

/*
 * Drop a reference on the owner for (@ctx, @type), freeing it when the last
 * reference goes away.
 */
static void damon_pmu_owner_put(struct damon_ctx *ctx, u32 type)
{
	struct damon_pmu_owner *owner, *tmp;

	spin_lock(&damon_pmu_owner_lock);
	list_for_each_entry_safe(owner, tmp, &damon_pmu_owner_list, node) {
		if (owner->pmu_type == type &&
		    atomic_long_read(&owner->owner_ctx) == (long)ctx) {
			if (atomic_dec_and_test(&owner->refcount)) {
				list_del(&owner->node);
				spin_unlock(&damon_pmu_owner_lock);
				kfree(owner);
				return;
			}
			break;
		}
	}
	spin_unlock(&damon_pmu_owner_lock);
}

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
	 * The "armed" reference: the NMI handler takes a reference after
	 * verifying ctx != NULL.  Teardown drops this and waits for zero.
	 */
	refcount_set(&event->refcount, 1);

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

	/*
	 * A system-wide PMU is a single hardware unit rather than a per-CPU
	 * counter, so it needs exactly one counter: the cpuhp fan-out below
	 * would run one redundant sampler per CPU against the one device and
	 * corrupt its shared state.  Pin that counter to a fixed online CPU
	 * and bypass cpuhp.
	 *
	 * The pin must name a real CPU (>= 0) because the PMU is
	 * perf_invalid_context, for which cpu = -1 routes to task context.
	 * If that CPU is later offlined, perf core either migrates the event
	 * to another CPU (scoped PMUs, via perf_pmu_migrate_context) where it
	 * keeps sampling, or moves it to PERF_EVENT_STATE_DEAD (scope-NONE
	 * PMUs) where it stops; in both cases the event struct is not freed
	 * (DAMON holds a reference from creation), so perf->single_event
	 * remains a valid pointer and teardown's disable/release below is
	 * safe.  Sampling is best-effort, so either outcome is acceptable.
	 */
	if (event->attr.single_instance) {
		struct perf_event_attr attr;
		int cpu = cpumask_first(cpu_online_mask);

		/*
		 * Defensive: the boot CPU cannot go offline, so this should
		 * never trigger, but a bogus CPU would corrupt the perf_event
		 * creation below.
		 */
		if (WARN_ON_ONCE(cpu >= nr_cpu_ids)) {
			err = -ENODEV;
			goto release_owner;
		}

		damon_perf_event_init_attr(event, &attr);
		/*
		 * Pass @event (not a probe_idx cookie) as the overflow
		 * context: damon_perf_overflow() reads event->ctx via
		 * smp_load_acquire() for the teardown race barrier, same as
		 * the per-CPU path (damon_perf_cpu_online()).
		 */
		perf->single_event = perf_event_create_kernel_counter(&attr,
				cpu, NULL, damon_perf_overflow, event);
		if (IS_ERR(perf->single_event)) {
			err = PTR_ERR(perf->single_event);
			perf->single_event = NULL;
			pr_warn("damon-perf: single-instance event create failed: %d\n",
				err);
			goto free_perf;
		}
		perf_event_enable(perf->single_event);
		/*
		 * Ownership is already held via the per-PMU owner->refcount
		 * acquired at the top of setup; the single-instance path shares
		 * that slot, so no separate refcount is taken here.  Teardown
		 * releases it through the same owner list as the per-CPU path.
		 */
		return 0;
	}

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

	/*
	 * The event was never armed (e.g. a param_ctx descriptor discarded
	 * after a failed commit): just free the descriptor.
	 */
	if (!perf) {
		kfree(event);
		return;
	}

	/*
	 * Signal in-flight NMI overflow handlers to drop samples before
	 * tearing down the perf events and freeing their backing state.
	 * Pairs with smp_load_acquire(&event->ctx) in damon_perf_overflow().
	 */
	smp_store_release(&event->ctx, NULL);

	/*
	 * cpuhp_state_remove_instance() disables and releases each CPU's perf
	 * event; once it returns, no new overflow can be delivered for this
	 * event.  The single-instance path bypassed cpuhp, so release its one
	 * counter directly; the event struct stays valid even if its pinned
	 * CPU went offline (see the setup comment).
	 */
	if (perf->single_event) {
		perf_event_disable(perf->single_event);
		perf_event_release_kernel(perf->single_event);
	} else {
		cpuhp_state_remove_instance(damon_perf_cpuhp_state,
					    &event->hlist_node);
		free_percpu(perf->event);
	}
	kfree(perf);
	event->priv = NULL;

	/*
	 * Drop the "armed" reference and wait for in-flight NMI handlers.
	 * A handler that passed the ctx check before we NULLed it holds a
	 * reference; we spin (not sleep — teardown can run in atomic
	 * context) until all such handlers complete.  New handlers cannot
	 * start: ctx is NULL and the perf events are disabled.
	 */
	refcount_dec(&event->refcount);
	while (refcount_read(&event->refcount) != 0)
		cpu_relax();

	/* Release per-PMU ownership when the last probe for this
	 * ctx/PMU-type pair is torn down.  kfree() under a non-irq spinlock
	 * in process context is safe.
	 */
	damon_pmu_owner_put(ctx, event->attr.type);

	/* teardown owns the event allocation */
	kfree(event);
}
EXPORT_SYMBOL_GPL(damon_perf_probe_teardown);

/**
 * damon_perf_probe_rearm - replace an armed perf event with a new one.
 * @ctx:   DAMON context that owns the probe.
 * @probe: the damon_probe being re-armed.
 * @old:   the currently armed descriptor (freed).
 * @new:   the new descriptor; on success ownership moves to the probe,
 *         on failure the caller retains it.
 *
 * The probe keeps its ctx->probes list position across the swap, so setup()
 * assigns @new the same probe_idx slot @old used; the NMI handler's
 * probe_hits[] indexing is undisturbed.
 *
 * Returns 0 on success, negative errno on failure.  On failure nothing is
 * left armed: the caller should clear the probe's event_driven flag.
 */
int damon_perf_probe_rearm(struct damon_ctx *ctx, struct damon_probe *probe,
			   struct damon_perf_probe_event *old,
			   struct damon_perf_probe_event *new)
{
	int err;
	bool owner_held;

	if (new->attr.type == old->attr.type) {
		/*
		 * Same PMU: hold an extra owner reference across the
		 * teardown/setup pair so the PMU claim never drops to zero
		 * and a concurrent turn-on cannot steal it in between.
		 */
		owner_held = damon_pmu_owner_get(ctx, old->attr.type);
		damon_perf_probe_teardown(ctx, old);
		err = damon_perf_probe_setup(ctx, probe, new);
		if (owner_held)
			damon_pmu_owner_put(ctx, old->attr.type);
	} else {
		/*
		 * Different PMU: there is no ownership to preserve across
		 * the swap, so release the old claim before acquiring the
		 * new one.
		 */
		damon_perf_probe_teardown(ctx, old);
		err = damon_perf_probe_setup(ctx, probe, new);
	}
	return err;
}
EXPORT_SYMBOL_GPL(damon_perf_probe_rearm);

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
