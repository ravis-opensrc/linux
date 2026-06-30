// SPDX-License-Identifier: GPL-2.0
#include <linux/bitops.h>
#include <linux/jump_label.h>
#include <linux/kstrtox.h>
#include <linux/migrate.h>
#include <linux/migrate_copy_offload.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/static_call.h>
#include <linux/string.h>
#include <linux/sysfs.h>

DEFINE_STATIC_KEY_FALSE(migrate_offload_enabled);
DEFINE_SRCU(migrate_offload_srcu);
DEFINE_STATIC_CALL(migrate_offload_batch_copy_fn, migrate_folios_mc_copy);

static DEFINE_MUTEX(migrator_mutex);
static const struct migrator *active_migrator;

/*
 * The active migrator's reason mask. This is the single source of truth
 * for which reasons are offloaded.
 */
static unsigned long active_reason_mask;

bool migrate_should_offload(int reason)
{
	if (!static_branch_unlikely(&migrate_offload_enabled))
		return false;

	return READ_ONCE(active_reason_mask) & BIT(reason);
}

/**
 * migrate_offload_reason_mask_parse - parse migration reason mask.
 * @buf: input string, either a number (hex/decimal, e.g. "0x101") or a
 *	comma-separated list of reason names (see migrate_reason_names[]),
 *	e.g. "compaction,demotion". The tokens "all" and "none" select
 *	every or no reason.
 * @maskp: parsed mask, clamped to MIGRATE_OFFLOAD_REASONS_ALLOWED.
 *
 * Return: 0 on success, -EINVAL on an unknown name, -ENOMEM on OOM.
 */
int migrate_offload_reason_mask_parse(const char *buf, unsigned long *maskp)
{
	unsigned long mask;
	char *copy, *p, *tok;
	int ret = 0;

	/* A plain number is treated as a raw mask. */
	if (!kstrtoul(buf, 0, &mask))
		goto done;

	copy = kstrdup(buf, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	mask = 0;
	p = strim(copy);

	while ((tok = strsep(&p, ",")) != NULL) {
		int i;

		tok = strim(tok);
		if (!*tok)
			continue;
		if (!strcmp(tok, "none")) {
			mask = 0;
			continue;
		}
		if (!strcmp(tok, "all")) {
			mask = MIGRATE_OFFLOAD_REASONS_ALLOWED;
			continue;
		}
		i = match_string(migrate_reason_names, MR_TYPES, tok);
		if (i < 0) {
			ret = -EINVAL;
			break;
		}
		mask |= BIT(i);
	}
	kfree(copy);
	if (ret)
		return ret;
done:
	*maskp = mask & MIGRATE_OFFLOAD_REASONS_ALLOWED;
	return 0;
}
EXPORT_SYMBOL_GPL(migrate_offload_reason_mask_parse);

/**
 * migrate_offload_reason_mask_format - render a reason mask as names.
 * @buf: output buffer from kernel_param_ops get.
 * @mask: reason mask to render.
 *
 * Emits a comma-separated list of reason names, or "none".
 *
 * Return: number of bytes written to @buf.
 */
int migrate_offload_reason_mask_format(char *buf, unsigned long mask)
{
	int len = 0, i;

	for_each_set_bit(i, &mask, MR_TYPES)
		len += sysfs_emit_at(buf, len, "%s%s",
				     len ? "," : "", migrate_reason_names[i]);
	if (!len)
		return sysfs_emit(buf, "none\n");

	len += sysfs_emit_at(buf, len, "\n");
	return len;
}
EXPORT_SYMBOL_GPL(migrate_offload_reason_mask_format);

/*
 * Hand the batch to the registered migrator. The migrator may decline
 * (typically based on batch size), in which case the move phase falls
 * back to per-folio CPU copy.
 */
int migrate_offload_batch_copy(struct list_head *dst_batch,
		struct list_head *src_batch, unsigned int nr_batch)
{
	int idx, rc;

	idx = srcu_read_lock(&migrate_offload_srcu);
	rc = static_call(migrate_offload_batch_copy_fn)(dst_batch, src_batch, nr_batch);
	srcu_read_unlock(&migrate_offload_srcu, idx);
	return rc;
}

/**
 * migrate_offload_set_reason_mask - update the active migrator's reason mask.
 * @m: migrator (must be the currently active one).
 * @mask: new reason mask.
 *
 * Return: 0 on success, -EINVAL if @m is NULL or not the active migrator.
 */
int migrate_offload_set_reason_mask(const struct migrator *m, unsigned long mask)
{
	if (!m)
		return -EINVAL;

	mask &= MIGRATE_OFFLOAD_REASONS_ALLOWED;

	mutex_lock(&migrator_mutex);
	if (active_migrator != m) {
		mutex_unlock(&migrator_mutex);
		return -EINVAL;
	}
	WRITE_ONCE(active_reason_mask, mask);
	mutex_unlock(&migrator_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(migrate_offload_set_reason_mask);

/**
 * migrate_offload_register - register a batch-copy provider for page migration.
 * @m: migrator to install.
 * @reason_mask: initial set of BIT(MR_*) reasons to offload, can be changed
 *	later via migrate_offload_set_reason_mask().
 *
 * Only one provider can be active at a time, returns -EBUSY if another migrator
 * is already registered.
 *
 * Return: 0 on success, negative errno on failure.
 */
int migrate_offload_register(const struct migrator *m, unsigned long reason_mask)
{
	unsigned long mask;
	int ret = 0;

	if (!m || !m->offload_copy)
		return -EINVAL;

	mask = reason_mask & MIGRATE_OFFLOAD_REASONS_ALLOWED;

	mutex_lock(&migrator_mutex);
	if (active_migrator) {
		ret = -EBUSY;
		goto unlock;
	}

	/* @owner is NULL for built-in (=y) drivers; nothing to ref. */
	if (m->owner && !try_module_get(m->owner)) {
		ret = -ENODEV;
		goto unlock;
	}

	WRITE_ONCE(active_reason_mask, mask);
	static_call_update(migrate_offload_batch_copy_fn, m->offload_copy);
	active_migrator = m;
	static_branch_enable(&migrate_offload_enabled);

unlock:
	mutex_unlock(&migrator_mutex);

	if (ret)
		pr_err("migrate_offload: %s: failed to register (%d)\n", m->name, ret);
	else
		pr_info("migrate_offload: enabled by %s (reason_mask=0x%lx)\n",
			m->name, mask);
	return ret;
}
EXPORT_SYMBOL_GPL(migrate_offload_register);

/**
 * migrate_offload_unregister - unregister the active batch-copy provider.
 * @m: migrator to remove (must be the currently active one).
 *
 * Reverts static_call targets and waits for SRCU grace period so that
 * no in-flight migration is still calling the driver functions before
 * releasing the module.
 *
 * Return: 0 on success, negative errno on failure.
 */
int migrate_offload_unregister(const struct migrator *m)
{
	struct module *owner;

	if (!m)
		return -EINVAL;

	mutex_lock(&migrator_mutex);
	if (active_migrator != m) {
		mutex_unlock(&migrator_mutex);
		return -EINVAL;
	}

	/*
	 * Disable the static branch first so new migrate_pages_batch() calls
	 * cannot enter the batch path.
	 */
	static_branch_disable(&migrate_offload_enabled);
	WRITE_ONCE(active_reason_mask, 0);
	static_call_update(migrate_offload_batch_copy_fn, migrate_folios_mc_copy);
	owner = active_migrator->owner;
	active_migrator = NULL;
	mutex_unlock(&migrator_mutex);

	/* Wait for all in-flight callers to finish before module_put(). */
	synchronize_srcu(&migrate_offload_srcu);
	if (owner)
		module_put(owner);

	pr_info("migrate_offload: disabled by %s\n", m->name);
	return 0;
}
EXPORT_SYMBOL_GPL(migrate_offload_unregister);
