// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON virtual AUX source
 *
 * A software PMU that writes address records into a perf AUX buffer, plus the
 * DAMON AUX backend that parses them.  Together they exercise the AUX report
 * path - record production, buffer parsing, tail release and report crediting
 * - on systems with no AUX-capable sampling hardware.
 *
 * The PMU emits records describing accesses to a caller-supplied address
 * range.  The range, the sub-range to emit most records into, and the process
 * the addresses belong to are configured through debugfs, so a test defines
 * what the source reports and then checks that DAMON acts on it.
 */

#include <linux/damon.h>
#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "aux_backend.h"
#include "perf_source.h"

/*
 * One AUX record: a page-aligned virtual address and its access kind.  The
 * size is a power of two that divides PAGE_SIZE, so a record never straddles
 * a page boundary of the AUX buffer and both sides can address records by
 * index.
 */
struct damon_vaux_record {
	u64 magic;
	u64 vaddr;
	u32 is_write;
	u32 pid;
	u64 reserved;
} __packed;

#define DAMON_VAUX_MAGIC	0x5641555852454331ULL
#define DAMON_VAUX_RECSZ	sizeof(struct damon_vaux_record)
#define DAMON_VAUX_PAGES	16
#define DAMON_VAUX_SIZE		(DAMON_VAUX_PAGES * PAGE_SIZE)
#define DAMON_VAUX_BATCH	64

/* Address range the emitted records refer to, all set through debugfs. */
static u64 damon_vaux_base;
static u64 damon_vaux_size;
static u64 damon_vaux_hot_base;
static u64 damon_vaux_hot_size;
static u32 damon_vaux_hot_pct = 90;
static u32 damon_vaux_pid;
static u32 damon_vaux_period_us = 1000;

/* Records produced and records parsed, exported through debugfs. */
static atomic_long_t damon_vaux_produced;
static atomic_long_t damon_vaux_parsed;
static atomic_long_t damon_vaux_rejected;

static struct pmu damon_vaux_pmu;
static int damon_vaux_pmu_type = -1;

/*
 * Per-CPU source state.  The producer work appends records to this CPU's AUX
 * buffer and re-arms itself; the drain callback parses from @aux_tail up to
 * the published head into @buf.  Both run on this CPU.
 */
struct damon_vaux_cpu {
	struct delayed_work produce;
	struct perf_event *event;
	int cpu;
	bool running;
	unsigned long aux_tail;
	void *buf;
};

static DEFINE_PER_CPU(struct damon_vaux_cpu, damon_vaux_cpus);

static u64 damon_vaux_pick_vaddr(void)
{
	u64 off, span;

	/*
	 * Emit into the hot sub-range with probability hot_pct, so a consumer
	 * can be checked for ranking that sub-range above the rest.
	 */
	if (damon_vaux_hot_size &&
	    (get_random_u32() % 100) < damon_vaux_hot_pct) {
		span = damon_vaux_hot_size >> PAGE_SHIFT;
		if (!span)
			return damon_vaux_hot_base;
		off = get_random_u32() % span;
		return damon_vaux_hot_base + (off << PAGE_SHIFT);
	}

	span = damon_vaux_size >> PAGE_SHIFT;
	if (!span)
		return damon_vaux_base;
	off = get_random_u32() % span;
	return damon_vaux_base + (off << PAGE_SHIFT);
}

/*
 * Append up to @nr records to this CPU's AUX buffer, as a hardware AUX PMU
 * writes its trace stream.  perf_aux_output_begin() hands back the private
 * cookie setup_aux() returned - for this PMU the array of AUX pages - and
 * reports the space available before the consumer's tail in handle.size; it
 * returns NULL once the buffer is full, which stops production until the
 * consumer releases space by advancing the tail.
 */
static void damon_vaux_produce_records(struct perf_event *event,
				       unsigned int nr)
{
	struct perf_output_handle handle;
	struct damon_vaux_record rec;
	unsigned long off;
	unsigned int i;
	void **pages;

	if (!damon_vaux_size)
		return;		/* address range not configured yet */

	pages = perf_aux_output_begin(&handle, event);
	if (!pages)
		return;

	for (i = 0; i < nr; i++) {
		if (handle.size < DAMON_VAUX_RECSZ * (i + 1))
			break;

		rec.magic = DAMON_VAUX_MAGIC;
		rec.vaddr = damon_vaux_pick_vaddr();
		rec.is_write = get_random_u32() & 1;
		rec.pid = damon_vaux_pid;
		rec.reserved = 0;

		off = (handle.head + i * DAMON_VAUX_RECSZ) %
			DAMON_VAUX_SIZE;
		memcpy(pages[off >> PAGE_SHIFT] + offset_in_page(off), &rec,
		       DAMON_VAUX_RECSZ);
	}

	/*
	 * Publish the records only after they are all written, so a consumer
	 * observing the new head sees complete data.
	 */
	perf_aux_output_end(&handle, i * DAMON_VAUX_RECSZ);
	atomic_long_add(i, &damon_vaux_produced);
}

static void damon_vaux_produce_fn(struct work_struct *work)
{
	struct damon_vaux_cpu *vc =
		container_of(to_delayed_work(work), struct damon_vaux_cpu,
			     produce);
	unsigned long delay;

	if (!vc->running || !vc->event)
		return;

	damon_vaux_produce_records(vc->event, DAMON_VAUX_BATCH);

	delay = usecs_to_jiffies(damon_vaux_period_us);
	queue_delayed_work_on(vc->cpu, system_percpu_wq, &vc->produce,
			      delay ? : 1);
}

/* ---- software PMU ---- */

static int damon_vaux_event_init(struct perf_event *event)
{
	if (event->attr.type != damon_vaux_pmu_type)
		return -ENOENT;
	return 0;
}

static void damon_vaux_event_read(struct perf_event *event) { }

static int damon_vaux_event_add(struct perf_event *event, int mode)
{
	return 0;
}

static void damon_vaux_event_del(struct perf_event *event, int mode) { }

static void damon_vaux_event_start(struct perf_event *event, int mode) { }

static void damon_vaux_event_stop(struct perf_event *event, int mode) { }

/*
 * The AUX pages are written by damon_vaux_produce_records() through the
 * handle perf hands out, so there is no hardware mapping to build and the
 * pages array itself is a sufficient private cookie.
 */
static void *damon_vaux_setup_aux(struct perf_event *event, void **pages,
				  int nr_pages, bool overwrite)
{
	return pages;
}

static void damon_vaux_free_aux(void *priv) { }

/* ---- DAMON AUX backend ---- */

static bool damon_vaux_match_pmu(struct perf_event *perf_event)
{
	return perf_event->pmu == &damon_vaux_pmu;
}

static int damon_vaux_backend_init(struct damon_perf_probe_event *event,
				   int cpu, struct perf_event *perf_event)
{
	struct damon_vaux_cpu *vc = per_cpu_ptr(&damon_vaux_cpus, cpu);
	int err;

	err = perf_event_setup_aux(perf_event, DAMON_VAUX_PAGES, 0);
	if (err)
		return err;

	vc->buf = kzalloc(DAMON_VAUX_RECSZ, GFP_KERNEL);
	if (!vc->buf) {
		perf_event_release_aux(perf_event);
		return -ENOMEM;
	}
	vc->aux_tail = 0;
	vc->event = perf_event;
	vc->cpu = cpu;
	vc->running = true;

	/* Produce on the CPU whose buffer this is, as a per-CPU PMU would. */
	INIT_DELAYED_WORK(&vc->produce, damon_vaux_produce_fn);
	queue_delayed_work_on(cpu, system_percpu_wq, &vc->produce,
			      usecs_to_jiffies(damon_vaux_period_us) ? : 1);

	cpumask_set_cpu(cpu, &event->aux_cpumask);
	return 0;
}

static void damon_vaux_backend_cleanup(struct damon_perf_probe_event *event,
				       int cpu)
{
	struct damon_vaux_cpu *vc = per_cpu_ptr(&damon_vaux_cpus, cpu);

	/*
	 * Clear the mask first so a concurrent drain stops selecting this CPU,
	 * then stop the producer before releasing the buffer it writes into.
	 */
	cpumask_clear_cpu(cpu, &event->aux_cpumask);
	vc->running = false;
	cancel_delayed_work_sync(&vc->produce);

	if (vc->event) {
		perf_event_release_aux(vc->event);
		vc->event = NULL;
	}
	kfree(vc->buf);
	vc->buf = NULL;
}

/*
 * Parse this CPU's AUX buffer into DAMON access reports.
 *
 * The tail is advanced only over records that damon_report_access() accepted.
 * On a rejected record the parse stops with the tail pointing at it, so the
 * AUX space holding it is not released and the record is parsed again on the
 * next drain.
 */
static unsigned int damon_vaux_backend_drain(struct damon_perf_probe_event *event,
					     int cpu)
{
	const size_t rsz = DAMON_VAUX_RECSZ;
	struct damon_vaux_cpu *vc = per_cpu_ptr(&damon_vaux_cpus, cpu);
	struct damon_vaux_record *rec = vc->buf;
	struct perf_event *perf_event = vc->event;
	unsigned long head, tail;
	unsigned int drained = 0;

	if (!rec || !perf_event)
		return 0;

	head = perf_event_aux_head(perf_event);
	tail = vc->aux_tail;

	while (tail + rsz <= head) {
		struct damon_access_report report = {
			.size = PAGE_SIZE,
			.cpu = cpu,
			.probe_idx = event->probe_idx,
			.ctx = event->ctx,
		};

		if (perf_event_aux_copy(perf_event, tail, tail + rsz, rec) < 0)
			break;

		if (rec->magic != DAMON_VAUX_MAGIC) {
			/* Not a record boundary; step to the next one. */
			tail += rsz;
			continue;
		}

		report.vaddr = rec->vaddr;
		report.is_write = !!rec->is_write;
		report.tid = rec->pid;

		if (!damon_report_access(&report)) {
			atomic_long_inc(&damon_vaux_rejected);
			break;
		}

		tail += rsz;
		drained++;
	}

	if (tail != vc->aux_tail) {
		vc->aux_tail = tail;
		perf_event_aux_tail_set(perf_event, tail);
	}
	atomic_long_add(drained, &damon_vaux_parsed);
	return drained;
}

static const struct damon_perf_backend_ops damon_vaux_ops = {
	.name = "vaux",
	.flags = DAMON_PERF_BACKEND_AUX,
	.match_pmu = damon_vaux_match_pmu,
	.init = damon_vaux_backend_init,
	.cleanup = damon_vaux_backend_cleanup,
	.drain = damon_vaux_backend_drain,
};

/* ---- debugfs configuration ---- */

static struct dentry *damon_vaux_debugfs_dir;

static int damon_vaux_produced_get(void *data, u64 *val)
{
	*val = atomic_long_read((atomic_long_t *)data);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(damon_vaux_counter_fops, damon_vaux_produced_get,
			 NULL, "%llu\n");

static void damon_vaux_debugfs_init(void)
{
	struct dentry *d;

	d = debugfs_create_dir("damon_vaux", NULL);
	damon_vaux_debugfs_dir = d;

	debugfs_create_u64("base", 0644, d, &damon_vaux_base);
	debugfs_create_u64("size", 0644, d, &damon_vaux_size);
	debugfs_create_u64("hot_base", 0644, d, &damon_vaux_hot_base);
	debugfs_create_u64("hot_size", 0644, d, &damon_vaux_hot_size);
	debugfs_create_u32("hot_pct", 0644, d, &damon_vaux_hot_pct);
	debugfs_create_u32("pid", 0644, d, &damon_vaux_pid);
	debugfs_create_u32("period_us", 0644, d, &damon_vaux_period_us);
	debugfs_create_u32("pmu_type", 0444, d, &damon_vaux_pmu_type);

	debugfs_create_file("produced", 0444, d, &damon_vaux_produced,
			    &damon_vaux_counter_fops);
	debugfs_create_file("parsed", 0444, d, &damon_vaux_parsed,
			    &damon_vaux_counter_fops);
	debugfs_create_file("rejected", 0444, d, &damon_vaux_rejected,
			    &damon_vaux_counter_fops);
}

static int __init damon_vaux_init(void)
{
	int ret;

	memset(&damon_vaux_pmu, 0, sizeof(damon_vaux_pmu));
	damon_vaux_pmu.event_init = damon_vaux_event_init;
	damon_vaux_pmu.add = damon_vaux_event_add;
	damon_vaux_pmu.del = damon_vaux_event_del;
	damon_vaux_pmu.start = damon_vaux_event_start;
	damon_vaux_pmu.stop = damon_vaux_event_stop;
	damon_vaux_pmu.read = damon_vaux_event_read;
	damon_vaux_pmu.setup_aux = damon_vaux_setup_aux;
	damon_vaux_pmu.free_aux = damon_vaux_free_aux;
	damon_vaux_pmu.capabilities = PERF_PMU_CAP_ITRACE;
	damon_vaux_pmu.task_ctx_nr = perf_sw_context;

	ret = perf_pmu_register(&damon_vaux_pmu, "damon_vaux", -1);
	if (ret)
		return ret;
	damon_vaux_pmu_type = damon_vaux_pmu.type;

	ret = damon_perf_aux_register_backend(&damon_vaux_ops);
	if (ret) {
		perf_pmu_unregister(&damon_vaux_pmu);
		damon_vaux_pmu_type = -1;
		return ret;
	}

	damon_vaux_debugfs_init();
	pr_info("damon_vaux: pmu type %d\n", damon_vaux_pmu_type);
	return 0;
}

static void __exit damon_vaux_exit(void)
{
	debugfs_remove_recursive(damon_vaux_debugfs_dir);
	if (damon_vaux_pmu_type >= 0)
		perf_pmu_unregister(&damon_vaux_pmu);
	damon_vaux_pmu_type = -1;
}

module_init(damon_vaux_init);
module_exit(damon_vaux_exit);
