/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MIGRATE_COPY_OFFLOAD_H
#define _LINUX_MIGRATE_COPY_OFFLOAD_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/migrate_mode.h>

struct list_head;
struct module;

#define MIGRATOR_NAME_LEN 32

/*
 * Reasons that may ever be offloaded. A driver's mask is clamped to this.
 * Allow all reasons by default.
 */
#define MIGRATE_OFFLOAD_REASONS_ALLOWED		(BIT(MR_TYPES) - 1)

/**
 * struct migrator - batch-copy provider for page migration.
 * @name: name of the provider.
 * @offload_copy: copy @folio_cnt folios from @src_list to @dst_list.
 *
 *	The migrator may inspect @folio_cnt to decide whether the batch
 *	is worth offloading, e.g. skip when the batch is too small to
 *	amortize setup cost.
 *	The callback must set FOLIO_CONTENT_COPIED in dst->migrate_info
 *	for each successfully copied destination folio.
 *	Folios without this marker are copied via per-folio CPU copy in
 *	the move phase.
 *
 * @owner: module providing the migrator. NULL for built-in (=y) drivers.
 */
struct migrator {
	char name[MIGRATOR_NAME_LEN];
	int (*offload_copy)(struct list_head *dst_list, struct list_head *src_list,
			unsigned int folio_cnt);
	struct module *owner;
};

#ifdef CONFIG_MIGRATION_COPY_OFFLOAD
int migrate_offload_register(const struct migrator *m, unsigned long reason_mask);
int migrate_offload_unregister(const struct migrator *m);
int migrate_offload_set_reason_mask(const struct migrator *m, unsigned long mask);
int migrate_offload_reason_mask_parse(const char *buf, unsigned long *maskp);
int migrate_offload_reason_mask_format(char *buf, unsigned long mask);
bool migrate_should_offload(int reason);
int migrate_offload_batch_copy(struct list_head *dst_batch,
		struct list_head *src_batch, unsigned int nr_batch);
#else
static inline int migrate_offload_register(const struct migrator *m,
		unsigned long reason_mask) { return -EOPNOTSUPP; }
static inline int migrate_offload_unregister(const struct migrator *m) { return -EOPNOTSUPP; }
static inline int migrate_offload_set_reason_mask(const struct migrator *m,
		unsigned long mask) { return -EOPNOTSUPP; }
static inline int migrate_offload_reason_mask_parse(const char *buf,
		unsigned long *maskp) { return -EOPNOTSUPP; }
static inline int migrate_offload_reason_mask_format(char *buf,
		unsigned long mask) { return -EOPNOTSUPP; }
static inline bool migrate_should_offload(int reason) { return false; }
static inline int migrate_offload_batch_copy(struct list_head *dst_batch,
		struct list_head *src_batch, unsigned int nr_batch)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_MIGRATE_COPY_OFFLOAD_H */
