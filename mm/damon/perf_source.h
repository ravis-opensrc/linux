/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON perf-event source - public interface
 *
 * Callers that register perf-event probes include this header.
 */

#ifndef _DAMON_PERF_SOURCE_H
#define _DAMON_PERF_SOURCE_H

#ifdef CONFIG_DAMON_PERF_SOURCE

#include <linux/cpumask.h>
#include <linux/damon.h>
#include <linux/perf_event.h>
#include <linux/workqueue.h>

#include "aux_backend.h"

/*
 * PMU event attributes for a perf-event probe.  A subset of perf_event_attr
 * chosen at probe creation time to select the PMU (AMD IBS, Intel PEBS, ...)
 * and its sampling parameters.
 */
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
	bool single_instance;	/* system-wide PMU: open one counter, not per-CPU */
	u64 sample_freq;
	u64 sample_period;
	u32 wakeup_events;
	u32 precise_ip;
};

struct damon_perf_probe_event;

/*
 * Per-CPU drain work for an AUX backend.  damon_report_access() enqueues into
 * the report ring of the calling CPU, so a CPU's AUX buffer is parsed on that
 * CPU via queue_work_on().
 */
struct damon_aux_drain_work {
	struct work_struct work;
	struct damon_perf_probe_event *event;
	int cpu;
	unsigned int nr;	/* records drained by the last run */
};

struct damon_perf_probe_event {
	struct damon_perf_event_attr attr;
	struct damon_ctx *ctx;	/* owning ctx for ring routing; set at setup */
	void *priv;		/* struct damon_perf_probe_state * */
	struct hlist_node hlist_node;
	int probe_idx;		/* index into probe_hits[]; set at registration */
	/* AUX backend state; ops == NULL for overflow-handler PMUs */
	const struct damon_perf_backend_ops *ops;
	struct damon_aux_drain_work __percpu *aux_work;
	cpumask_t aux_cpumask;	/* CPUs with initialised AUX resources */
};

int damon_perf_probe_setup(struct damon_ctx *ctx,
			   struct damon_probe *probe,
			   struct damon_perf_probe_event *event);
void damon_perf_probe_teardown(struct damon_ctx *ctx,
			       struct damon_perf_probe_event *event);
struct damon_probe *damon_perf_probe_alloc(unsigned int weight);

#endif /* CONFIG_DAMON_PERF_SOURCE */
#endif /* _DAMON_PERF_SOURCE_H */
