/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON perf AUX backend interface
 *
 * An AUX backend consumes a PMU's trace buffer instead of an overflow
 * handler: the PMU writes records into the AUX ring, and the backend
 * parses them into DAMON access reports at drain time.
 */
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
 * @name: backend name.
 * @flags: bitmask of DAMON_PERF_BACKEND_* flags.
 * @match_pmu: return true when this backend claims @perf_event.
 * @init: allocate per-event, per-CPU resources.
 * @cleanup: release the resources initialised for one CPU.
 * @drain: parse pending AUX data into DAMON access reports.
 *
 * All callbacks run in process context.  The caller serialises resource
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
void damon_perf_aux_free(struct damon_perf_probe_event *event);
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

static inline void damon_perf_aux_free(struct damon_perf_probe_event *event)
{
}
#endif

#endif /* _DAMON_AUX_BACKEND_H */
