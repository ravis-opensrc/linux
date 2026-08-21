/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DAMON_AUX_BACKEND_H
#define _DAMON_AUX_BACKEND_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/perf_event.h>
#include <linux/types.h>

struct damon_ctx;
struct damon_perf_probe_event;

/**
 * struct damon_perf_backend_ops - PMU-specific AUX backend operations
 * @name: Human-readable backend name.
 * @flags: Bitmask of DAMON_PERF_BACKEND_* flags.
 * @match_pmu: Return true when this backend claims @perf_event.
 * @init: Allocate per-event, per-CPU resources.
 * @cleanup: Release resources initialized for one CPU.
 * @arm: Prepare the AUX producer before perf_event_enable().
 * @disarm: Quiesce backend state after perf_event_disable().
 * @drain: Parse pending AUX data into DAMON access reports.
 *
 * All callbacks run in process context.  The caller serializes resource
 * lifetime against CPU hotplug and invokes @drain only for CPUs recorded
 * in damon_perf_probe_event::aux_cpumask.
 */
struct damon_perf_backend_ops {
	const char *name;
	u32 flags;
	bool (*match_pmu)(struct perf_event *perf_event);
	int (*init)(struct damon_perf_probe_event *event, int cpu,
		    struct perf_event *perf_event);
	void (*cleanup)(struct damon_perf_probe_event *event, int cpu);
	int (*arm)(struct damon_perf_probe_event *event, int cpu);
	void (*disarm)(struct damon_perf_probe_event *event, int cpu);
	unsigned int (*drain)(struct damon_perf_probe_event *event, int cpu);
};

#define DAMON_PERF_BACKEND_AUX	BIT(0)

#ifdef CONFIG_DAMON_PERF_AUX
void damon_perf_aux_drain(struct damon_ctx *ctx);
int damon_perf_aux_register_backend(const struct damon_perf_backend_ops *ops);
const struct damon_perf_backend_ops *
damon_perf_aux_find_backend(struct perf_event *perf_event);
void damon_perf_aux_select(struct damon_perf_probe_event *event,
			   struct perf_event *perf_event);
#else
static inline void damon_perf_aux_drain(struct damon_ctx *ctx)
{
}

static inline int
damon_perf_aux_register_backend(const struct damon_perf_backend_ops *ops)
{
	return -EOPNOTSUPP;
}

static inline const struct damon_perf_backend_ops *
damon_perf_aux_find_backend(struct perf_event *perf_event)
{
	return NULL;
}

static inline void damon_perf_aux_select(struct damon_perf_probe_event *event,
					 struct perf_event *perf_event)
{
}
#endif

#endif /* _DAMON_AUX_BACKEND_H */
