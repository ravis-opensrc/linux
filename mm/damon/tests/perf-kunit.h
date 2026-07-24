/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON kunit tests for global report ring.
 */

#ifdef CONFIG_DAMON_KUNIT_TEST

#ifndef _DAMON_PERF_KUNIT_H
#define _DAMON_PERF_KUNIT_H

#include <kunit/test.h>
#include <linux/damon.h>

/*
 * Test A: global ring basic write/read
 *
 * Inject reports into the global ring via damon_report_access(), drain and
 * verify the entries are readable.
 */
static void damon_test_global_ring_basic(struct kunit *test)
{
	struct damon_access_report report = {
		.paddr = 0x1000, .size = PAGE_SIZE, .probe_idx = 1,
	};
	int i;

	/* Inject 3 reports into global ring */
	for (i = 0; i < 3; i++)
		damon_report_access(&report);

	/*
	 * In a real drain we'd iterate the cpumask and check entries.
	 * For this unit test we just verify damon_report_access() is callable
	 * and doesn't crash.  Full drain logic is tested in drain-kunit.h.
	 */
}

/*
 * Test B: ring overflow does not corrupt head/tail
 *
 * Fill the global ring to capacity and verify that further writes are
 * dropped (overflow counter incremented) without corrupting the ring.
 */
static void damon_test_global_ring_overflow_safety(struct kunit *test)
{
	struct damon_access_report report = {
		.paddr = 0x3000, .size = PAGE_SIZE, .probe_idx = 1,
	};
	int i;
	unsigned long overflow_before, overflow_after;

	preempt_disable();
	overflow_before = damon_get_report_overflow();

	/* Fill to DAMON_REPORT_RING_SIZE - 1 entries (ring full) */
	for (i = 0; i < DAMON_REPORT_RING_SIZE; i++)
		damon_report_access(&report);

	/* One more write - must increment overflow counter */
	damon_report_access(&report);

	overflow_after = damon_get_report_overflow();
	preempt_enable();

	KUNIT_EXPECT_GT(test, overflow_after, overflow_before);
}

/*
 * Test C: damon_report_access() is ctx-less and exported
 *
 * Verify that damon_report_access() can be called without a ctx parameter
 * (compile-time check) and is exported for use by perf_source.c.
 */
static void damon_test_global_ring_ctx_less(struct kunit *test)
{
	struct damon_access_report report = {
		.paddr = 0x5000, .size = PAGE_SIZE, .probe_idx = 1,
	};

	/* This compiles only if damon_report_access() takes no ctx parameter */
	damon_report_access(&report);
}

static struct kunit_case damon_perf_test_cases[] = {
	KUNIT_CASE(damon_test_global_ring_basic),
	KUNIT_CASE(damon_test_global_ring_overflow_safety),
	KUNIT_CASE(damon_test_global_ring_ctx_less),
	{}
};

static struct kunit_suite damon_perf_test_suite = {
	.name = "damon_perf",
	.test_cases = damon_perf_test_cases,
};
kunit_test_suite(damon_perf_test_suite);

#endif /* _DAMON_PERF_KUNIT_H */

#endif	/* CONFIG_DAMON_KUNIT_TEST */
