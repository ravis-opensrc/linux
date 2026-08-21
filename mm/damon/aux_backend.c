// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Perf AUX Backend - Generic Drain Scheduler
 *
 * Provides damon_perf_aux_drain() which is called by kdamond_fn() each
 * tick before the report ring drain.  It iterates all perf probes on the
 * context and for any probe whose backend ops carry DAMON_PERF_BACKEND_AUX,
 * invokes the per-CPU drain callback to parse PMU records and feed them
 * into damon_report_access().
 *
 * Also provides the auto-selection helper that assigns a backend to an
 * event based on the PMU object of the created perf_event.
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/damon.h>
#include <linux/list.h>

#include "aux_backend.h"
#include "perf_source.h"

/* Registered backends (populated at initcall time). */
#define AUX_BACKEND_MAX 4

static const struct damon_perf_backend_ops *aux_backends[AUX_BACKEND_MAX];
static int nr_aux_backends;

/**
 * damon_perf_aux_register_backend - Register an AUX backend ops table
 * @ops: Backend callbacks to register.
 *
 * Called at initcall time by each backend.  Returns 0 on success,
 * -ENOSPC if the static table is full.
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
 * damon_perf_aux_find_backend - Find the backend that claims the PMU
 * @perf_event: Created perf event whose PMU should be matched.
 *
 * Returns the ops table whose match_pmu() claims @perf_event->pmu
 * (exact PMU object comparison), or NULL if no backend matches.
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

/**
 * damon_perf_aux_select - Auto-select a backend for @event
 * @event: DAMON probe event that will own the selected backend.
 * @perf_event: Created perf event used for capability and PMU matching.
 *
 * Called from damon_perf_cpu_online() after the first perf_event is
 * created.  Checks the PMU capabilities of the created event; if it has
 * PERF_PMU_CAP_ITRACE, looks up a registered AUX backend that claims
 * the PMU object of the created event.
 * Overflow-handler PMUs (IBS, PEBS, generic counters) keep ops == NULL.
 */
void damon_perf_aux_select(struct damon_perf_probe_event *event,
			   struct perf_event *perf_event)
{
	if (event->ops)
		return;		/* already assigned */

	if (!(perf_event->pmu->capabilities & PERF_PMU_CAP_ITRACE))
		return;		/* not an ITRACE / AUX PMU */

	event->ops = damon_perf_aux_find_backend(perf_event);
}
EXPORT_SYMBOL_GPL(damon_perf_aux_select);

/**
 * damon_perf_aux_drain - Drain all AUX backends into the report ring
 * @ctx: DAMON context whose AUX probes should be drained.
 *
 * Must be called BEFORE the report ring drain each tick so that
 * freshly-parsed records are available for it.
 * Also called at kdamond stop for a final flush.
 */
void damon_perf_aux_drain(struct damon_ctx *ctx)
{
	struct damon_perf_probe_event *event;
	struct damon_probe *p;
	int cpu;

	/*
	 * Hold the CPU hotplug read lock so that a concurrent CPU offline
	 * callback cannot free the per-CPU backend resources (the parse
	 * window, AUX buffer) while drain is accessing them.  The offline
	 * path runs under the write-side hotplug lock and clears
	 * aux_cpumask before freeing, so once cpus_read_lock() is held any
	 * CPU still in the mask has live resources.
	 */
	cpus_read_lock();
	damon_for_each_probe(p, ctx) {
		event = p->perf_priv;
		if (!event || !event->ops ||
		    !(event->ops->flags & DAMON_PERF_BACKEND_AUX))
			continue;
		if (!event->ops->drain)
			continue;

		/* Only CPUs with initialized AUX resources. */
		for_each_cpu(cpu, &event->aux_cpumask)
			event->ops->drain(event, cpu);
	}
	cpus_read_unlock();
}
EXPORT_SYMBOL_GPL(damon_perf_aux_drain);
