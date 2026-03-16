/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM damon

#if !defined(_TRACE_DAMON_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DAMON_H

#include <linux/damon.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

TRACE_EVENT(damos_stat_after_apply_interval,

	TP_PROTO(unsigned int context_idx, unsigned int scheme_idx,
		struct damos_stat *stat),

	TP_ARGS(context_idx, scheme_idx, stat),

	TP_STRUCT__entry(
		__field(unsigned int, context_idx)
		__field(unsigned int, scheme_idx)
		__field(unsigned long, nr_tried)
		__field(unsigned long, sz_tried)
		__field(unsigned long, nr_applied)
		__field(unsigned long, sz_applied)
		__field(unsigned long, sz_ops_filter_passed)
		__field(unsigned long, qt_exceeds)
		__field(unsigned long, nr_snapshots)
	),

	TP_fast_assign(
		__entry->context_idx = context_idx;
		__entry->scheme_idx = scheme_idx;
		__entry->nr_tried = stat->nr_tried;
		__entry->sz_tried = stat->sz_tried;
		__entry->nr_applied = stat->nr_applied;
		__entry->sz_applied = stat->sz_applied;
		__entry->sz_ops_filter_passed = stat->sz_ops_filter_passed;
		__entry->qt_exceeds = stat->qt_exceeds;
		__entry->nr_snapshots = stat->nr_snapshots;
	),

	TP_printk("ctx_idx=%u scheme_idx=%u nr_tried=%lu sz_tried=%lu "
			"nr_applied=%lu sz_tried=%lu sz_ops_filter_passed=%lu "
			"qt_exceeds=%lu nr_snapshots=%lu",
			__entry->context_idx, __entry->scheme_idx,
			__entry->nr_tried, __entry->sz_tried,
			__entry->nr_applied, __entry->sz_applied,
			__entry->sz_ops_filter_passed, __entry->qt_exceeds,
			__entry->nr_snapshots)
);

TRACE_EVENT(damos_esz,

	TP_PROTO(unsigned int context_idx, unsigned int scheme_idx,
		unsigned long esz),

	TP_ARGS(context_idx, scheme_idx, esz),

	TP_STRUCT__entry(
		__field(unsigned int, context_idx)
		__field(unsigned int, scheme_idx)
		__field(unsigned long, esz)
	),

	TP_fast_assign(
		__entry->context_idx = context_idx;
		__entry->scheme_idx = scheme_idx;
		__entry->esz = esz;
	),

	TP_printk("ctx_idx=%u scheme_idx=%u esz=%lu",
			__entry->context_idx, __entry->scheme_idx,
			__entry->esz)
);

TRACE_EVENT_CONDITION(damos_before_apply,

	TP_PROTO(unsigned int context_idx, unsigned int scheme_idx,
		unsigned int target_idx, struct damon_region *r,
		unsigned int nr_regions, bool do_trace),

	TP_ARGS(context_idx, scheme_idx, target_idx, r, nr_regions, do_trace),

	TP_CONDITION(do_trace),

	TP_STRUCT__entry(
		__field(unsigned int, context_idx)
		__field(unsigned int, scheme_idx)
		__field(unsigned long, target_idx)
		__field(unsigned long, start)
		__field(unsigned long, end)
		__field(unsigned int, nr_accesses)
		__field(unsigned int, age)
		__field(unsigned int, nr_regions)
	),

	TP_fast_assign(
		__entry->context_idx = context_idx;
		__entry->scheme_idx = scheme_idx;
		__entry->target_idx = target_idx;
		__entry->start = r->ar.start;
		__entry->end = r->ar.end;
		__entry->nr_accesses = r->nr_accesses_bp / 10000;
		__entry->age = r->age;
		__entry->nr_regions = nr_regions;
	),

	TP_printk("ctx_idx=%u scheme_idx=%u target_idx=%lu nr_regions=%u %lu-%lu: %u %u",
			__entry->context_idx, __entry->scheme_idx,
			__entry->target_idx, __entry->nr_regions,
			__entry->start, __entry->end,
			__entry->nr_accesses, __entry->age)
);

TRACE_EVENT(damon_monitor_intervals_tune,

	TP_PROTO(unsigned long sample_us),

	TP_ARGS(sample_us),

	TP_STRUCT__entry(
		__field(unsigned long, sample_us)
	),

	TP_fast_assign(
		__entry->sample_us = sample_us;
	),

	TP_printk("sample_us=%lu", __entry->sample_us)
);

TRACE_EVENT(damon_aggregated,

	TP_PROTO(unsigned int target_id, struct damon_region *r,
		unsigned int nr_regions),

	TP_ARGS(target_id, r, nr_regions),

	TP_STRUCT__entry(
		__field(unsigned long, target_id)
		__field(unsigned int, nr_regions)
		__field(unsigned long, start)
		__field(unsigned long, end)
		__field(unsigned int, nr_accesses)
		__field(unsigned int, age)
	),

	TP_fast_assign(
		__entry->target_id = target_id;
		__entry->nr_regions = nr_regions;
		__entry->start = r->ar.start;
		__entry->end = r->ar.end;
		__entry->nr_accesses = r->nr_accesses;
		__entry->age = r->age;
	),

	TP_printk("target_id=%lu nr_regions=%u %lu-%lu: %u %u",
			__entry->target_id, __entry->nr_regions,
			__entry->start, __entry->end,
			__entry->nr_accesses, __entry->age)
);

TRACE_EVENT(damos_eligible_raw,

	TP_PROTO(unsigned int context_idx, unsigned int scheme_idx, int nid,
		unsigned long node_eligible, unsigned long total_eligible),

	TP_ARGS(context_idx, scheme_idx, nid, node_eligible, total_eligible),

	TP_STRUCT__entry(
		__field(unsigned int, context_idx)
		__field(unsigned int, scheme_idx)
		__field(int, nid)
		__field(unsigned long, node_eligible)
		__field(unsigned long, total_eligible)
	),

	TP_fast_assign(
		__entry->context_idx = context_idx;
		__entry->scheme_idx = scheme_idx;
		__entry->nid = nid;
		__entry->node_eligible = node_eligible;
		__entry->total_eligible = total_eligible;
	),

	TP_printk("ctx_idx=%u scheme_idx=%u nid=%d node_eligible=%lu total=%lu",
			__entry->context_idx, __entry->scheme_idx, __entry->nid,
			__entry->node_eligible, __entry->total_eligible)
);

TRACE_EVENT(damos_eligible_effective,

	TP_PROTO(unsigned int context_idx, unsigned int scheme_idx, int nid,
		unsigned long detected, unsigned long effective,
		long delta, bool cache_active),

	TP_ARGS(context_idx, scheme_idx, nid, detected, effective, delta,
		cache_active),

	TP_STRUCT__entry(
		__field(unsigned int, context_idx)
		__field(unsigned int, scheme_idx)
		__field(int, nid)
		__field(unsigned long, detected)
		__field(unsigned long, effective)
		__field(long, delta)
		__field(bool, cache_active)
	),

	TP_fast_assign(
		__entry->context_idx = context_idx;
		__entry->scheme_idx = scheme_idx;
		__entry->nid = nid;
		__entry->detected = detected;
		__entry->effective = effective;
		__entry->delta = delta;
		__entry->cache_active = cache_active;
	),

	TP_printk("ctx_idx=%u scheme_idx=%u nid=%d detected=%lu effective=%lu delta=%ld active=%d",
			__entry->context_idx, __entry->scheme_idx, __entry->nid,
			__entry->detected, __entry->effective,
			__entry->delta, __entry->cache_active)
);

TRACE_EVENT(damos_cache_update,

	TP_PROTO(unsigned int context_idx, unsigned int scheme_idx, int nid,
		unsigned long sz_applied, unsigned int slot,
		long delta_in_slot),

	TP_ARGS(context_idx, scheme_idx, nid, sz_applied, slot, delta_in_slot),

	TP_STRUCT__entry(
		__field(unsigned int, context_idx)
		__field(unsigned int, scheme_idx)
		__field(int, nid)
		__field(unsigned long, sz_applied)
		__field(unsigned int, slot)
		__field(long, delta_in_slot)
	),

	TP_fast_assign(
		__entry->context_idx = context_idx;
		__entry->scheme_idx = scheme_idx;
		__entry->nid = nid;
		__entry->sz_applied = sz_applied;
		__entry->slot = slot;
		__entry->delta_in_slot = delta_in_slot;
	),

	TP_printk("ctx_idx=%u scheme_idx=%u nid=%d sz_applied=%lu slot=%u delta=%ld",
			__entry->context_idx, __entry->scheme_idx, __entry->nid,
			__entry->sz_applied, __entry->slot,
			__entry->delta_in_slot)
);

TRACE_EVENT(damos_cache_advance,

	TP_PROTO(unsigned int context_idx, unsigned int scheme_idx, int nid,
		unsigned int current_slot, long total_delta, bool cache_active),

	TP_ARGS(context_idx, scheme_idx, nid, current_slot, total_delta,
		cache_active),

	TP_STRUCT__entry(
		__field(unsigned int, context_idx)
		__field(unsigned int, scheme_idx)
		__field(int, nid)
		__field(unsigned int, current_slot)
		__field(long, total_delta)
		__field(bool, cache_active)
	),

	TP_fast_assign(
		__entry->context_idx = context_idx;
		__entry->scheme_idx = scheme_idx;
		__entry->nid = nid;
		__entry->current_slot = current_slot;
		__entry->total_delta = total_delta;
		__entry->cache_active = cache_active;
	),

	TP_printk("ctx_idx=%u scheme_idx=%u nid=%d slot=%u total_delta=%ld active=%d",
			__entry->context_idx, __entry->scheme_idx, __entry->nid,
			__entry->current_slot, __entry->total_delta,
			__entry->cache_active)
);

#endif /* _TRACE_DAMON_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
