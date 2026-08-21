// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON perf AUX backend - generic drain scheduler
 *
 * Provides damon_perf_aux_drain(), called from the kdamond loop each tick
 * before the report-ring drain.  It walks the context's probes and, for any
 * probe whose backend ops carry DAMON_PERF_BACKEND_AUX, invokes the per-CPU
 * drain callback to parse PMU records into damon_report_access().
 *
 * damon_report_access() enqueues into the report ring of the CPU that calls
 * it, so each CPU's AUX buffer is parsed on that CPU: the drain callback is
 * dispatched with queue_work_on() and the results are collected once all
 * dispatched work has completed.  Per-tick ring capacity therefore scales
 * with the number of CPUs being drained.
 *
 * Also provides the backend auto-selection helper, which assigns a backend
 * to a probe event based on the PMU of the created perf_event.
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/damon.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "aux_backend.h"
#include "perf_source.h"

/* Registered backends, populated at initcall time. */
#define AUX_BACKEND_MAX 4

static const struct damon_perf_backend_ops *aux_backends[AUX_BACKEND_MAX];
static int nr_aux_backends;

/**
 * damon_perf_aux_register_backend - register an AUX backend ops table
 * @ops: backend callbacks to register.
 *
 * Called at initcall time by each backend.  Returns 0 on success, -ENOSPC if
 * the static table is full.
 */
int damon_perf_aux_register_backend(const struct damon_perf_backend_ops *ops)
{
	if (nr_aux_backends >= AUX_BACKEND_MAX)
		return -ENOSPC;

	aux_backends[nr_aux_backends++] = ops;
	return 0;
}
EXPORT_SYMBOL_GPL(damon_perf_aux_register_backend);

/**
 * damon_perf_aux_find_backend - find the backend that claims a PMU
 * @perf_event: created perf event whose PMU should be matched.
 *
 * Returns the ops table whose match_pmu() claims @perf_event, or NULL if no
 * registered backend matches.
 */
const struct damon_perf_backend_ops *
damon_perf_aux_find_backend(struct perf_event *perf_event)
{
	int i;

	for (i = 0; i < nr_aux_backends; i++) {
		if (aux_backends[i]->match_pmu &&
		    aux_backends[i]->match_pmu(perf_event))
			return aux_backends[i];
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(damon_perf_aux_find_backend);

static void damon_aux_drain_work_fn(struct work_struct *work)
{
	struct damon_aux_drain_work *dw =
		container_of(work, struct damon_aux_drain_work, work);

	dw->nr = dw->event->ops->drain(dw->event, dw->cpu);
}

/**
 * damon_perf_aux_select - select a backend for @event
 * @event: DAMON probe event that will own the selected backend.
 * @perf_event: created perf event used for capability and PMU matching.
 *
 * Called after the first perf_event for the probe is created.  A PMU that
 * writes into an AUX buffer advertises PERF_PMU_CAP_ITRACE; for such a PMU,
 * look up a registered backend that claims it and allocate the per-CPU drain
 * work used to dispatch that backend's drain callback.
 *
 * PMUs that report through the overflow handler keep ops == NULL.
 */
void damon_perf_aux_select(struct damon_perf_probe_event *event,
			   struct perf_event *perf_event)
{
	const struct damon_perf_backend_ops *ops;
	struct damon_aux_drain_work *dw;
	int cpu;

	if (event->ops)
		return;		/* already assigned */

	if (!(perf_event->pmu->capabilities & PERF_PMU_CAP_ITRACE))
		return;		/* not an AUX-writing PMU */

	ops = damon_perf_aux_find_backend(perf_event);
	if (!ops)
		return;

	event->aux_work = alloc_percpu(struct damon_aux_drain_work);
	if (!event->aux_work)
		return;

	for_each_possible_cpu(cpu) {
		dw = per_cpu_ptr(event->aux_work, cpu);
		INIT_WORK(&dw->work, damon_aux_drain_work_fn);
		dw->event = event;
		dw->cpu = cpu;
	}

	event->ops = ops;
}
EXPORT_SYMBOL_GPL(damon_perf_aux_select);

/**
 * damon_perf_aux_free - release the per-CPU drain work for @event
 * @event: DAMON probe event being torn down.
 *
 * Any dispatched drain work has already completed, because
 * damon_perf_aux_drain() flushes every work item it queues before returning.
 */
void damon_perf_aux_free(struct damon_perf_probe_event *event)
{
	free_percpu(event->aux_work);
	event->aux_work = NULL;
	event->ops = NULL;
}
EXPORT_SYMBOL_GPL(damon_perf_aux_free);

static unsigned int damon_perf_aux_drain_event(struct damon_perf_probe_event *event)
{
	struct damon_aux_drain_work *dw;
	unsigned int drained = 0;
	int cpu;

	/*
	 * Dispatch every CPU's drain first, then collect, so the parses run
	 * concurrently rather than one CPU at a time.  system_percpu_wq is
	 * per-CPU, so queue_work_on() runs each item on the CPU whose buffer it
	 * parses; an unbound workqueue would not honour that.
	 */
	for_each_cpu(cpu, &event->aux_cpumask) {
		dw = per_cpu_ptr(event->aux_work, cpu);
		dw->nr = 0;
		if (!queue_work_on(cpu, system_percpu_wq, &dw->work)) {
			/*
			 * Already queued from a previous tick that has not yet
			 * run; it will consume the pending records.
			 */
			continue;
		}
	}

	for_each_cpu(cpu, &event->aux_cpumask) {
		dw = per_cpu_ptr(event->aux_work, cpu);
		flush_work(&dw->work);
		drained += dw->nr;
	}

	return drained;
}

/**
 * damon_perf_aux_drain - drain a context's AUX backends into the report ring
 * @ctx: DAMON context whose AUX probes should be drained.
 *
 * Must be called before the report-ring drain each tick so that
 * freshly-parsed records are visible to it, and once more at kdamond stop as
 * a final flush.
 */
void damon_perf_aux_drain(struct damon_ctx *ctx)
{
	struct damon_perf_probe_event *event;
	struct damon_probe *p;

	/*
	 * Hold the CPU hotplug read lock so a concurrent offline callback
	 * cannot free a CPU's backend resources while they are being drained.
	 * The offline path clears the CPU from aux_cpumask before freeing, so
	 * any CPU still in the mask under this lock has live resources.
	 */
	cpus_read_lock();
	damon_for_each_probe(p, ctx) {
		event = p->perf_priv;
		if (!event || !event->ops || !event->aux_work)
			continue;
		if (!(event->ops->flags & DAMON_PERF_BACKEND_AUX))
			continue;
		if (!event->ops->drain)
			continue;

		damon_perf_aux_drain_event(event);
	}
	cpus_read_unlock();
}
EXPORT_SYMBOL_GPL(damon_perf_aux_drain);
