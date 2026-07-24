/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON kunit tests for the unified paddr/vaddr report drain path.
 *
 * Included at the bottom of core.c (after kdamond_check_reported_accesses
 * is defined) so the static function is visible.
 */

#ifdef CONFIG_DAMON_KUNIT_TEST

#ifndef _DAMON_DRAIN_KUNIT_H
#define _DAMON_DRAIN_KUNIT_H

#include <kunit/test.h>
#include <linux/damon.h>

/*
 * Reports are partitioned by probe_idx: probe_idx == DAMON_PROBE_IDX_NONE (0)
 * lands in the global page_fault ring; probe_idx >= 1 lands in the owning
 * context's per-context perf ring (ctx->perf_rings).  The drain dispatcher
 * kdamond_check_reported_accesses() drains the perf ring only for a ctx that
 * has event-driven probes.
 *
 * Attach a dummy event-driven probe AND allocate the ctx's per-ctx perf ring
 * so the ctx both drains the perf ring and has ring storage for injected
 * probe_idx>=1 reports.  In a live run damon_perf_probe_setup() allocates the
 * ring; kunit has no real perf event, so it allocates directly.  Returns
 * 0/-ENOMEM.
 */
static int damon_test_attach_perf_probe(struct damon_ctx *ctx)
{
	struct damon_probe *p = damon_new_probe();
	int err;

	if (!p)
		return -ENOMEM;
	p->event_driven = true;
	damon_add_probe(ctx, p);

	err = damon_ctx_alloc_perf_ring(ctx);
	if (err)
		return err;
	return 0;
}

/*
 * Test A: vaddr entry with matching tid drains correctly.
 *
 * Create a vaddr ctx with target pid=current, region [0x1000, 0x2000).
 * Inject entry: paddr=0, vaddr=0x1500, tid=current tgid, probe_idx=1, ctx=ctx.
 * After drain: probe_hits[0]==1 (probe_idx 1 stored 0-based), samples_drained
 * increments.
 */
static void damon_test_unified_vaddr_match(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0,
		.vaddr     = 0x1500,
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = get_pid(task_tgid(current));
	if (!t->pid) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "pid alloc failed");
	}
	rep.tid = task_tgid_vnr(current);
	rep.ctx = ctx;	/* route to this ctx's per-ctx perf ring */

	/*
	 * Region must fully contain the report [vaddr, vaddr + size): a report
	 * straddling the region end is rejected by the drain (correctly).  With
	 * vaddr=0x1500 and size=PAGE_SIZE the region must reach >= 0x2500.
	 */
	r = damon_new_region(0x1000, 0x3000);
	if (!r) {
		put_pid(t->pid);
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_drained();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_drained();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 1);
	KUNIT_EXPECT_GT(test, after, before);

	damon_destroy_ctx(ctx);
}

/*
 * Test B: vaddr entry with wrong tid is dropped.
 *
 * Same setup but inject with tid=9999 (no matching target).
 * probe_hits[0]==0 (probe_idx 1 stored 0-based), samples_no_region increments.
 */
static void damon_test_unified_vaddr_tid_mismatch(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0,
		.vaddr     = 0x1500,
		.tid       = 9999,	/* deliberately wrong */
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = get_pid(task_tgid(current));
	if (!t->pid) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "pid alloc failed");
	}
	rep.ctx = ctx;

	/* Wide enough to contain the report; the tid mismatch is the sole reject reason. */
	r = damon_new_region(0x1000, 0x3000);
	if (!r) {
		put_pid(t->pid);
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_no_region();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_no_region();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 0);
	KUNIT_EXPECT_GT(test, after, before);

	damon_destroy_ctx(ctx);
}

/*
 * Test C: paddr entry drains correctly (no tid filter for paddr ops).
 *
 * Create a paddr ctx (no pid), region [0x10000, 0x20000).
 * Inject: paddr=0x15000, vaddr=0, probe_idx=1, ctx=ctx.
 * After drain: probe_hits[0]==1 (probe_idx 1 stored 0-based).
 */
static void damon_test_unified_paddr_no_regression(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0x15000,
		.vaddr     = 0,
		.tid       = 0,
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;	/* paddr target: no pid */
	rep.ctx = ctx;

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_drained();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_drained();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 1);
	KUNIT_EXPECT_GT(test, after, before);

	damon_destroy_ctx(ctx);
}

/*
 * Test pf-ring credit: a page_fault ctx drains the global pf ring.
 *
 * Create a paddr ctx with page_fault primitive enabled (no probes), region
 * [0x10000, 0x20000).  Inject probe_idx=0 (DAMON_PROBE_IDX_NONE) paddr entry;
 * such reports carry no ctx and land in the global pf ring.  After drain via
 * the dispatcher: samples_drained increments (access rate credited) and
 * probe_hits stays 0 (no probe attribution for probe_idx 0).
 */
static void damon_test_ring0_pagefault_credit(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0x15000,
		.vaddr     = 0,
		.tid       = 0,
		.probe_idx = 0,	/* DAMON_PROBE_IDX_NONE -> global pf ring */
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	/* page_fault primitive: routes drain to the global pf ring. */
	ctx->sample_control.primitives_enabled.page_table = false;
	ctx->sample_control.primitives_enabled.page_fault = true;

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;	/* paddr target: no pid */

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_drained();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_drained();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 0);	/* probe_idx 0: no probe_hits */
	KUNIT_EXPECT_GT(test, after, before);

	damon_destroy_ctx(ctx);
}

/*
 * Test pf-ring cold-demote signal: complements ring0_pagefault_credit
 * (which proves the HOT side).  This proves the COLD side.
 *
 * Create a paddr ctx with the page_fault primitive enabled (no probes) and one
 * target with TWO regions: HOT [0x10000, 0x20000) and COLD [0x20000, 0x30000).
 * Prime both regions to the same nonzero nr_accesses so aging is observable.
 * Inject a single page_fault report (probe_idx=0 -> pf ring) hitting only the
 * HOT region, then run the drain + zero-access-report aging pass:
 *
 *   kdamond_check_reported_accesses() credits the HOT region: it gets a
 *   nr_accesses bump and access_reported=true; the COLD region is untouched.
 *
 *   kdamond_apply_zero_access_report() clears access_reported on the HOT
 *   region (keeping its nr_accesses) and, because the COLD region was NOT
 *   reported this tick, calls damon_update_region_access_rate(r, false) on it
 *   (no credit) so the COLD region does not advance.
 *
 * After the cycle the HOT region's nr_accesses has pulled ahead of the COLD
 * region's (HOT > COLD) -- the cold-demote signal a migrate_cold scheme acts
 * on -- and access_reported is false on both regions again.
 */
static void damon_test_ring0_pagefault_cold_demote(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *hot, *cold;
	struct damon_access_report rep = {
		.paddr     = 0x15000,	/* inside HOT region */
		.vaddr     = 0,
		.tid       = 0,
		.probe_idx = 0,	/* DAMON_PROBE_IDX_NONE -> global pf ring */
		.size      = PAGE_SIZE,
	};

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	/* page_fault primitive: routes drain to the global pf ring. */
	ctx->sample_control.primitives_enabled.page_table = false;
	ctx->sample_control.primitives_enabled.page_fault = true;

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;	/* paddr target: no pid */

	hot = damon_new_region(0x10000, 0x20000);
	if (!hot) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "hot region alloc failed");
	}
	damon_add_region(hot, t);

	cold = damon_new_region(0x20000, 0x30000);
	if (!cold) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "cold region alloc failed");
	}
	damon_add_region(cold, t);
	damon_add_target(ctx, t);

	/* Prime both regions to the same nonzero access rate. */
	hot->nr_accesses = 4;
	cold->nr_accesses = 4;

	/* Inject a pf-ring report hitting only the HOT region, then drain. */
	rep.report_jiffies = jiffies;
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);

	/* HOT reported this tick; COLD was not. */
	KUNIT_EXPECT_TRUE(test, hot->access_reported);
	KUNIT_EXPECT_FALSE(test, cold->access_reported);

	/* Aging pass: HOT keeps its (credited) rate; COLD is not credited. */
	kdamond_apply_zero_access_report(ctx);

	/* Cold-demote signal: HOT pulled ahead of COLD. */
	KUNIT_EXPECT_GT(test, hot->nr_accesses, cold->nr_accesses);
	/* access_reported cleared on both after the zero-report pass. */
	KUNIT_EXPECT_FALSE(test, hot->access_reported);
	KUNIT_EXPECT_FALSE(test, cold->access_reported);

	damon_destroy_ctx(ctx);
}

/*
 * Test perf-ring credit: a perf ctx drains its own per-ctx perf ring.
 *
 * Create a paddr ctx with an event-driven probe (which allocates the ctx's
 * per-ctx perf ring), region [0x10000, 0x20000).  Inject a probe_idx=1 paddr
 * entry tagged with ctx, so it lands in that ctx's perf ring.  After drain:
 * probe_hits[0]==1.
 */
static void damon_test_ring1_perf_credit(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0x15000,
		.vaddr     = 0,
		.tid       = 0,
		.probe_idx = 1,	/* -> per-ctx perf ring */
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;
	rep.ctx = ctx;

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_drained();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_drained();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 1);
	KUNIT_EXPECT_GT(test, after, before);

	damon_destroy_ctx(ctx);
}

/*
 * Test ring partition: a perf-only ctx does NOT consume a pf-ring entry.
 *
 * Inject a probe_idx=0 (global pf ring) entry, then drain with a perf-only ctx
 * (event probe, page_fault disabled).  The perf ctx drains only its per-ctx
 * perf ring, which is empty, so samples_drained must NOT change.  A subsequent
 * pf ctx drain consumes the entry (samples_drained increments), proving the
 * entry was partitioned into the global pf ring and left untouched by the perf
 * drain.
 */
static void damon_test_ring_partition(struct kunit *test)
{
	struct damon_ctx *perf_ctx, *pf_ctx;
	struct damon_target *t_perf, *t_pf;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0x15000,
		.vaddr     = 0,
		.tid       = 0,
		.probe_idx = 0,	/* DAMON_PROBE_IDX_NONE -> global pf ring */
		.size      = PAGE_SIZE,
	};
	unsigned long before, mid, after;

	perf_ctx = damon_new_ctx();
	if (!perf_ctx)
		kunit_skip(test, "perf ctx alloc failed");
	if (damon_test_attach_perf_probe(perf_ctx)) {
		damon_destroy_ctx(perf_ctx);
		kunit_skip(test, "perf probe alloc failed");
	}
	t_perf = damon_new_target();
	if (!t_perf) {
		damon_destroy_ctx(perf_ctx);
		kunit_skip(test, "perf target alloc failed");
	}
	t_perf->pid = NULL;
	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t_perf);
		damon_destroy_ctx(perf_ctx);
		kunit_skip(test, "perf region alloc failed");
	}
	damon_add_region(r, t_perf);
	damon_add_target(perf_ctx, t_perf);

	pf_ctx = damon_new_ctx();
	if (!pf_ctx) {
		damon_destroy_ctx(perf_ctx);
		kunit_skip(test, "pf ctx alloc failed");
	}
	pf_ctx->sample_control.primitives_enabled.page_table = false;
	pf_ctx->sample_control.primitives_enabled.page_fault = true;
	t_pf = damon_new_target();
	if (!t_pf) {
		damon_destroy_ctx(pf_ctx);
		damon_destroy_ctx(perf_ctx);
		kunit_skip(test, "pf target alloc failed");
	}
	t_pf->pid = NULL;
	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t_pf);
		damon_destroy_ctx(pf_ctx);
		damon_destroy_ctx(perf_ctx);
		kunit_skip(test, "pf region alloc failed");
	}
	damon_add_region(r, t_pf);
	damon_add_target(pf_ctx, t_pf);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_drained();
	damon_report_access(&rep);		/* lands in the global pf ring */
	kdamond_check_reported_accesses(perf_ctx); /* drains perf ring only */
	mid = damon_get_samples_drained();
	kdamond_check_reported_accesses(pf_ctx);   /* drains pf ring */
	after = damon_get_samples_drained();

	KUNIT_EXPECT_EQ(test, mid, before);	/* perf drain left pf entry */
	KUNIT_EXPECT_GT(test, after, mid);	/* pf drain consumed it */

	damon_destroy_ctx(pf_ctx);
	damon_destroy_ctx(perf_ctx);
}

/*
 * Test per-context perf ring isolation.
 *
 * Two independent perf ctxs (each with its own event-driven probe and its own
 * per-ctx perf ring) each receive one probe_idx=1 report tagged with their
 * respective ctx.  Each ctx must credit exactly its own report and see nothing
 * from the other: proof that perf reports route to the owning ctx's ring, and
 * that two perf-driven ctxs coexist without a shared ring or a cross-ctx owner
 * guard (unlike the global perf ring, which allowed only one perf drainer).
 */
static void damon_test_perf_per_ctx_isolation(struct kunit *test)
{
	struct damon_ctx *ctx_a, *ctx_b;
	struct damon_target *ta, *tb;
	struct damon_region *ra, *rb;
	struct damon_access_report rep_a = {
		.paddr = 0x15000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	struct damon_access_report rep_b = {
		.paddr = 0x35000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	int hits_a, hits_b;

	ctx_a = damon_new_ctx();
	ctx_b = damon_new_ctx();
	if (!ctx_a || !ctx_b) {
		if (ctx_a)
			damon_destroy_ctx(ctx_a);
		if (ctx_b)
			damon_destroy_ctx(ctx_b);
		kunit_skip(test, "ctx alloc failed");
	}
	if (damon_test_attach_perf_probe(ctx_a) ||
			damon_test_attach_perf_probe(ctx_b)) {
		damon_destroy_ctx(ctx_a);
		damon_destroy_ctx(ctx_b);
		kunit_skip(test, "perf probe alloc failed");
	}

	ta = damon_new_target();
	tb = damon_new_target();
	if (!ta || !tb) {
		if (ta)
			damon_free_target(ta);
		if (tb)
			damon_free_target(tb);
		damon_destroy_ctx(ctx_a);
		damon_destroy_ctx(ctx_b);
		kunit_skip(test, "target alloc failed");
	}
	ta->pid = NULL;
	tb->pid = NULL;

	ra = damon_new_region(0x10000, 0x20000);	/* holds rep_a paddr */
	rb = damon_new_region(0x30000, 0x40000);	/* holds rep_b paddr */
	if (!ra || !rb) {
		if (ra)
			damon_free_region(ra);
		if (rb)
			damon_free_region(rb);
		damon_free_target(ta);
		damon_free_target(tb);
		damon_destroy_ctx(ctx_a);
		damon_destroy_ctx(ctx_b);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(ra, ta);
	damon_add_target(ctx_a, ta);
	damon_add_region(rb, tb);
	damon_add_target(ctx_b, tb);

	/* Each report is tagged with its owning ctx. */
	rep_a.ctx = ctx_a;
	rep_b.ctx = ctx_b;
	rep_a.report_jiffies = jiffies;
	rep_b.report_jiffies = jiffies;

	/*
	 * Report into both ctx rings, then drain each ctx.  ctx_a must credit
	 * only rep_a; ctx_b must credit only rep_b -- no cross-talk, and no
	 * -EBUSY from a second perf drainer.
	 */
	damon_report_access(&rep_a);
	damon_report_access(&rep_b);
	kdamond_check_reported_accesses(ctx_a);
	kdamond_check_reported_accesses(ctx_b);

	hits_a = 0;
	damon_for_each_region(ra, ta)
		hits_a += ra->probe_hits[0];
	hits_b = 0;
	damon_for_each_region(rb, tb)
		hits_b += rb->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits_a, 1);	/* ctx_a credited its own report */
	KUNIT_EXPECT_EQ(test, hits_b, 1);	/* ctx_b credited its own report */

	damon_destroy_ctx(ctx_a);
	damon_destroy_ctx(ctx_b);
}

/*
 * Test pf-ring single-owner claim (page_fault only).
 *
 * The global pf ring is a destructive SPSC channel with a single owner:
 * two ctxs draining it in one batch must be rejected with -EBUSY.  The perf
 * ring has no such batch-claim owner (it is per-ctx), so only the pf owner
 * helper is exercised here.
 */
static void damon_test_pf_ring_owner_ebusy(struct kunit *test)
{
	struct damon_ctx *a, *b;
	struct damon_ctx *arr[2];
	struct damon_ctx *owner_pf;
	int err;

	a = damon_new_ctx();
	b = damon_new_ctx();
	if (!a || !b) {
		if (a)
			damon_destroy_ctx(a);
		if (b)
			damon_destroy_ctx(b);
		kunit_skip(test, "ctx alloc failed");
	}

	/* Both drain the global pf ring: same-ring collision -> -EBUSY. */
	a->sample_control.primitives_enabled.page_table = false;
	a->sample_control.primitives_enabled.page_fault = true;
	b->sample_control.primitives_enabled.page_table = false;
	b->sample_control.primitives_enabled.page_fault = true;
	arr[0] = a;
	arr[1] = b;
	owner_pf = NULL;
	err = damon_claim_ring_owner_start(arr, 2, damon_drains_ring_pf,
			&owner_pf);
	KUNIT_EXPECT_EQ(test, err, -EBUSY);

	/* Only a drains pf: single owner, claimed to a. */
	b->sample_control.primitives_enabled.page_fault = false;
	owner_pf = NULL;
	err = damon_claim_ring_owner_start(arr, 2, damon_drains_ring_pf,
			&owner_pf);
	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_PTR_EQ(test, owner_pf, a);

	damon_destroy_ctx(a);
	damon_destroy_ctx(b);
}

/*
 * Test the queued/dropped return value, and that a ring-full drop is counted
 * as ring-full rather than busy-guard.
 *
 * A per-context perf ring is private to its ctx, so a freshly created ctx
 * starts with an empty ring nobody else writes to and the counts are exact.
 * Preemption is held across the loop so every report targets the same CPU's
 * ring, per the SPSC invariant damon_report_access() documents.
 *
 * A ring holds DAMON_REPORT_RING_SIZE - 1 entries (one slot is kept empty to
 * distinguish full from empty), so exactly that many reports are queued and
 * every one after that is dropped.
 */
static void damon_test_report_return_value(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_access_report rep = {
		.paddr = 0x15000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	unsigned long full_before, busy_before;
	unsigned int queued = 0, dropped = 0;
	int i;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}
	rep.ctx = ctx;

	preempt_disable();
	full_before = damon_get_report_ring_full();
	busy_before = damon_get_report_busy_drop();

	/* One past capacity, so the last iteration must be a drop. */
	for (i = 0; i < DAMON_REPORT_RING_SIZE; i++) {
		if (damon_report_access(&rep))
			queued++;
		else
			dropped++;
	}
	preempt_enable();

	KUNIT_EXPECT_EQ(test, queued, (unsigned int)DAMON_REPORT_RING_SIZE - 1);
	KUNIT_EXPECT_EQ(test, dropped, 1u);
	/* No NMI nests here, so the drop must be the full ring. */
	KUNIT_EXPECT_GT(test, damon_get_report_ring_full(), full_before);
	KUNIT_EXPECT_EQ(test, damon_get_report_busy_drop(), busy_before);

	damon_destroy_ctx(ctx);
}

/*
 * Test that draining restores capacity: fill the ring, drain it via the
 * dispatcher, then report again and expect the report to be queued.
 */
static void damon_test_report_drain_restores_capacity(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr = 0x15000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	int i;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;
	rep.ctx = ctx;

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	/* Fill the ring: the last report is dropped. */
	preempt_disable();
	for (i = 0; i < DAMON_REPORT_RING_SIZE; i++)
		damon_report_access(&rep);
	KUNIT_EXPECT_FALSE(test, damon_report_access(&rep));
	preempt_enable();

	kdamond_check_reported_accesses(ctx);

	/* Capacity is back. */
	preempt_disable();
	KUNIT_EXPECT_TRUE(test, damon_report_access(&rep));
	preempt_enable();

	damon_destroy_ctx(ctx);
}

static struct kunit_case damon_drain_test_cases[] = {
	KUNIT_CASE(damon_test_unified_vaddr_match),
	KUNIT_CASE(damon_test_unified_vaddr_tid_mismatch),
	KUNIT_CASE(damon_test_unified_paddr_no_regression),
	KUNIT_CASE(damon_test_ring0_pagefault_credit),
	KUNIT_CASE(damon_test_ring0_pagefault_cold_demote),
	KUNIT_CASE(damon_test_ring1_perf_credit),
	KUNIT_CASE(damon_test_ring_partition),
	KUNIT_CASE(damon_test_perf_per_ctx_isolation),
	KUNIT_CASE(damon_test_pf_ring_owner_ebusy),
	KUNIT_CASE(damon_test_report_return_value),
	KUNIT_CASE(damon_test_report_drain_restores_capacity),
	{}
};

static struct kunit_suite damon_drain_test_suite = {
	.name = "damon_drain",
	.test_cases = damon_drain_test_cases,
};
kunit_test_suite(damon_drain_test_suite);

#endif /* _DAMON_DRAIN_KUNIT_H */

#endif	/* CONFIG_DAMON_KUNIT_TEST */
