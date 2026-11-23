// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI descriptor encoding.
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <kunit/test-bug.h>
#include <kunit/visibility.h>
#include <linux/bitfield.h>
#include <linux/bug.h>
#include <linux/math64.h>
#include <linux/range.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#include "hw.h"
#include "descriptor.h"

VISIBLE_IF_KUNIT int __must_check sdxi_encode_size32(u64 size, __le32 *dest)
{
	/*
	 * Sizes are encoded as value - 1:
	 * value    encoding
	 *     1           0
	 *     2           1
	 *   ...
	 *    4G  0xffffffff
	 * See SDXI 1.0 Table 6-8: DSC_DMAB_COPY Descriptor Format,
	 * for example.
	 */
	if (size > 0 && size <= SZ_4G) {
		*dest = cpu_to_le32((u32)(size - 1));
		return 0;
	}

	/* Inhibit warning stack traces while testing. */
	if (!kunit_get_current_test()) {
		WARN_ON_ONCE(size == 0);
		WARN_ON_ONCE(size > SZ_4G);
	}

	return -EINVAL;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_size32);

int sdxi_encode_copy(struct sdxi_desc *desc, const struct sdxi_copy *params)
{
	u64 csb_ptr;
	u32 opcode;
	__le32 size;
	int err;

	err = sdxi_encode_size32(params->len, &size);
	if (err)
		return err;
	/*
	 * Reject overlapping src and dst. "Software ... shall not
	 * overlap the source buffer, destination buffer, Atomic
	 * Return Data, or completion status block." - SDXI 1.0 5.6
	 * Memory Consistency Model
	 */
	if (range_overlaps(&(const struct range) {
				   .start = params->src,
				   .end   = params->src + params->len - 1,
			   },
			   &(const struct range) {
				   .start = params->dst,
				   .end   = params->dst + params->len - 1,
			   }))
		return -EINVAL;

	opcode = (FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_COPY) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_DMAB));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.copy = (typeof(desc->copy)) {
			.opcode = cpu_to_le32(opcode),
			.size = size,
			.akey0 = cpu_to_le16(params->src_akey),
			.akey1 = cpu_to_le16(params->dst_akey),
			.addr0 = cpu_to_le64(params->src),
			.addr1 = cpu_to_le64(params->dst),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_copy);

int sdxi_encode_repcopy(struct sdxi_desc *desc,
			const struct sdxi_repcopy *params)
{
	struct range src, dst;
	u64 total, nsize, num;
	u32 opcode;

	if (!IS_ALIGNED(params->src | params->dst | params->src_len, SZ_4K))
		return -EINVAL;
	if (params->src_len < SZ_4K || params->src_len > SZ_2M)
		return -EINVAL;
	if (!params->nr_copies || params->nr_copies > BIT_ULL(20))
		return -EINVAL;

	total = params->src_len * params->nr_copies;
	/* Neither range may wrap past the end of the address space. */
	src.start = params->src;
	dst.start = params->dst;
	if (check_add_overflow(params->src, params->src_len - 1, &src.end) ||
	    check_add_overflow(params->dst, total - 1, &dst.end))
		return -EINVAL;
	/* The source must not overlap the destination (SDXI 1.0 5.6). */
	if (range_overlaps(&src, &dst))
		return -EINVAL;

	/* Both fields are encoded as value - 1. */
	nsize = params->src_len / SZ_4K - 1;
	num = params->nr_copies - 1;

	opcode = (FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_REPCOPY) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_DMAB));

	*desc = (typeof(*desc)) {
		.repcopy = (typeof(desc->repcopy)) {
			.opcode = cpu_to_le32(opcode),
			.nsize = cpu_to_le32(FIELD_PREP(SDXI_DSC_REPCOPY_NSIZE, nsize)),
			.akey0 = cpu_to_le16(params->src_akey),
			.akey1 = cpu_to_le16(params->dst_akey),
			.addr0 = cpu_to_le64(FIELD_PREP(SDXI_DSC_REPCOPY_ADDR, params->src >> 12) |
					     FIELD_PREP(SDXI_DSC_REPCOPY_AZ, params->zero)),
			.addr1 = cpu_to_le64(FIELD_PREP(SDXI_DSC_REPCOPY_ADDR, params->dst >> 12)),
			.num = cpu_to_le32(FIELD_PREP(SDXI_DSC_REPCOPY_NUM, num)),
			.csb_ptr = cpu_to_le64(FIELD_PREP(SDXI_DSC_NP, 1)),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_repcopy);

/*
 * A repeat copy may not write more than the context's max_buffer
 * (2^(max_buffer + 21) bytes) nor more than 4 GiB (4 KiB * 2^20 copies).
 */
u64 sdxi_repcopy_max_dst(u8 max_buffer)
{
	return min_t(u64, (u64)SZ_2M << min_t(u8, max_buffer, 15), SZ_4G);
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_repcopy_max_dst);

int sdxi_plan_fill(dma_addr_t dst, u64 len, u8 max_buffer,
		   struct sdxi_fill_plan *plan)
{
	u64 rest, chunk = sdxi_repcopy_max_dst(max_buffer);

	if (!len || len > U64_MAX - dst)
		return -EINVAL;

	plan->head = min_t(u64, len, ALIGN(dst, SZ_4K) - dst);
	rest = len - plan->head;
	plan->body = ALIGN_DOWN(rest, SZ_4K);
	plan->tail = rest - plan->body;
	plan->chunk = chunk;
	plan->nr_body = DIV64_U64_ROUND_UP(plan->body, chunk);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_plan_fill);

int sdxi_encode_intr(struct sdxi_desc *desc,
		     const struct sdxi_intr *params)
{
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_INTR) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_INTR));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.intr = (typeof(desc->intr)) {
			.opcode = cpu_to_le32(opcode),
			.akey = cpu_to_le16(params->akey),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_intr);

int sdxi_encode_cxt_start(struct sdxi_desc *desc,
			  const struct sdxi_cxt_start *params)
{
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_FE, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_START_NM) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.cxt_start = (typeof(desc->cxt_start)) {
			.opcode = cpu_to_le32(opcode),
			.cxt_start = cpu_to_le16(params->range.cxt_start),
			.cxt_end = cpu_to_le16(params->range.cxt_end),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_start);

int sdxi_encode_cxt_stop(struct sdxi_desc *desc,
			  const struct sdxi_cxt_stop *params)
{
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_FE, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_STOP) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.cxt_stop = (typeof(desc->cxt_stop)) {
			.opcode = cpu_to_le32(opcode),
			.cxt_start = cpu_to_le16(params->range.cxt_start),
			.cxt_end = cpu_to_le16(params->range.cxt_end),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_stop);

int sdxi_encode_cxt_upd(struct sdxi_desc *desc,
			const struct sdxi_cxt_upd *params)
{
	u64 csb_ptr;
	u32 opcode;
	u8 cflags;

	opcode = (FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_UPD) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	cflags = FIELD_PREP(SDXI_DSC_CXT_UPD_PCD, params->level);

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.cxt_upd = (typeof(desc->cxt_upd)) {
			.opcode = cpu_to_le32(opcode),
			.cflags = cflags,
			.cxt_start = cpu_to_le16(params->range.cxt_start),
			.cxt_end = cpu_to_le16(params->range.cxt_end),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_upd);

int sdxi_encode_sync(struct sdxi_desc *desc, const struct sdxi_sync *params)
{
	u64 csb_ptr;
	u32 opcode;
	u8 cflags;

	opcode = (FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_SYNC) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	cflags = FIELD_PREP(SDXI_DSC_SYNC_FLT, params->filter);

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.sync = (typeof(desc->sync)) {
			.opcode = cpu_to_le32(opcode),
			.cflags = cflags,
			.cxt_start = cpu_to_le16(params->range.cxt_start),
			.cxt_end = cpu_to_le16(params->range.cxt_end),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_sync);
