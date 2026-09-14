/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON perf-event source - public interface
 *
 * Callers that register perf-event probes include this header.
 */

#ifndef _DAMON_PERF_SOURCE_H
#define _DAMON_PERF_SOURCE_H

#ifdef CONFIG_DAMON_PERF_SOURCE

#include <linux/damon.h>
#include <linux/perf_event.h>
#include <linux/refcount.h>

/* struct damon_perf_event_attr is defined in linux/damon.h. */

struct damon_perf_probe_event {
	struct damon_perf_event_attr attr;
	struct damon_ctx *ctx;	/* owning ctx for ring routing; set at setup */
	void *priv;		/* struct damon_perf_probe_state * */
	struct hlist_node hlist_node;
	int probe_idx;		/* 1-based index into probe_hits[]; set at setup */
	/*
	 * Lifetime: 1 at setup (the "armed" reference).  The NMI overflow
	 * handler takes a reference after verifying ctx != NULL; teardown
	 * drops the armed reference and spins until the count reaches zero
	 * before freeing.  This closes the cross-CPU race where an NMI on
	 * another CPU is inside the handler while teardown runs.
	 */
	refcount_t refcount;
};

int damon_perf_probe_setup(struct damon_ctx *ctx,
			   struct damon_probe *probe,
			   struct damon_perf_probe_event *event);
void damon_perf_probe_teardown(struct damon_ctx *ctx,
			       struct damon_perf_probe_event *event);
int damon_perf_probe_rearm(struct damon_ctx *ctx, struct damon_probe *probe,
			   struct damon_perf_probe_event *old,
			   struct damon_perf_probe_event *new);

#endif /* CONFIG_DAMON_PERF_SOURCE */
#endif /* _DAMON_PERF_SOURCE_H */
