// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI descriptor encoding tests.
 *
 * Copyright Advanced Micro Devices, Inc.
 *
 * Decode descriptors using packing layouts specified independently of the
 * production bitfield masks, allowing the tests to detect incorrect field
 * definitions.
 *
 * Defining SKIP_PACKING_CHECKS bypasses compile-time packing-layout validation
 * to reduce build time.
 */
#include <kunit/device.h>
#include <kunit/test-bug.h>
#include <kunit/test.h>
#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/packing.h>
#include <linux/sizes.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include "descriptor.h"

/* #define SKIP_PACKING_CHECKS */

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

enum {
	SDXI_PACKING_QUIRKS = QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST,
};


#define desc_field(_high, _low, _target_struct, _member) \
	PACKED_FIELD(_high, _low, _target_struct, _member)
#define desc_flag(_bit, _target_struct, _member) \
	desc_field(_bit, _bit, _target_struct, _member)

/* DMAB_COPY */
struct unpacked__copy {
	u32 size;
	u8 attr_src;
	u8 attr_dst;
	u16 akey0;
	u16 akey1;
	u64 addr0;
	u64 addr1;
};

#define copy_field(_high, _low, _member) \
	desc_field(_high, _low, struct unpacked__copy, _member)

static const struct packed_field_u16 copy_subfields[] = {
	copy_field(63, 32, size),
	copy_field(67, 64, attr_src),
	copy_field(71, 68, attr_dst),
	copy_field(111, 96, akey0),
	copy_field(127, 112, akey1),
	copy_field(191, 128, addr0),
	copy_field(255, 192, addr1),
};

/* DMAB_REPCOPY */
struct unpacked__repcopy {
	u16 nsize;
	u8 attr_src;
	u8 attr_dst;
	u16 akey0;
	u16 akey1;
	bool az;
	u64 addr0;
	u64 addr1;
	u32 num;
};

#define repcopy_field(_high, _low, _member) \
	desc_field(_high, _low, struct unpacked__repcopy, _member)

static const struct packed_field_u16 repcopy_subfields[] = {
	repcopy_field(52, 44, nsize),
	repcopy_field(67, 64, attr_src),
	repcopy_field(71, 68, attr_dst),
	repcopy_field(111, 96, akey0),
	repcopy_field(127, 112, akey1),
	repcopy_field(128, 128, az),
	repcopy_field(191, 140, addr0),
	repcopy_field(255, 204, addr1),
	repcopy_field(287, 268, num),
};

/* DSC_INTR */
struct unpacked__intr {
	u16 akey;
};

#define intr_field(_high, _low, _member) \
	desc_field(_high, _low, struct unpacked__intr, _member)

static const struct packed_field_u16 intr_subfields[] = {
	intr_field(111, 96, akey),
};

/* DSC_SYNC */
struct unpacked__sync {
	u8 flt;
	bool vf;
	u16 vf_num;
	u16 cxt_start;
	u16 cxt_end;
	u16 key_start;
	u16 key_end;
};

#define sync_field(_high, _low, _member) \
	desc_field(_high, _low, struct unpacked__sync, _member)
#define sync_flag(_bit, _member) sync_field(_bit, _bit, _member)

static const struct packed_field_u16 sync_subfields[] = {
	sync_field(34, 32, flt),
	sync_flag(47, vf),
	sync_field(63, 48, vf_num),
	sync_field(79, 64, cxt_start),
	sync_field(95, 80, cxt_end),
	sync_field(111, 96, key_start),
	sync_field(127, 112, key_end),
};

/* DSC_CXT_START */
struct unpacked__cxt_start {
	bool dv;
	bool vf;
	u16 vf_num;
	u16 cxt_start;
	u16 cxt_end;
	u64 db_value;
};

#define cxt_start_field(_high, _low, _member) \
	desc_field(_high, _low, struct unpacked__cxt_start, _member)
#define cxt_start_flag(_bit, _member) cxt_start_field(_bit, _bit, _member)

static const struct packed_field_u16 cxt_start_subfields[] = {
	cxt_start_flag(46, dv),
	cxt_start_flag(47, vf),
	cxt_start_field(63, 48, vf_num),
	cxt_start_field(79, 64, cxt_start),
	cxt_start_field(95, 80, cxt_end),
	cxt_start_field(191, 128, db_value),
};

/* DSC_CXT_STOP */
struct unpacked__cxt_stop {
	bool hs;
	bool vf;
	u16 vf_num;
	u16 cxt_start;
	u16 cxt_end;
};

#define cxt_stop_field(_high, _low, _member) \
	desc_field(_high, _low, struct unpacked__cxt_stop, _member)
#define cxt_stop_flag(_bit, _member) cxt_stop_field(_bit, _bit, _member)

static const struct packed_field_u16 cxt_stop_subfields[] = {
	cxt_stop_flag(45, hs),
	cxt_stop_flag(47, vf),
	cxt_stop_field(63, 48, vf_num),
	cxt_stop_field(79, 64, cxt_start),
	cxt_stop_field(95, 80, cxt_end),
};

/* DSC_CXT_UPD */
struct unpacked__cxt_upd {
	u8 pcd;
	bool vf;
	u16 vf_num;
	u16 cxt_start;
	u16 cxt_end;
};

#define cxt_upd_field(_high, _low, _member) \
	desc_field(_high, _low, struct unpacked__cxt_upd, _member)
#define cxt_upd_flag(_bit, _member) cxt_upd_field(_bit, _bit, _member)

static const struct packed_field_u16 cxt_upd_subfields[] = {
	cxt_upd_field(34, 32, pcd),
	cxt_upd_flag(47, vf),
	cxt_upd_field(63, 48, vf_num),
	cxt_upd_field(79, 64, cxt_start),
	cxt_upd_field(95, 80, cxt_end),
};

/* DSC_GENERIC */
struct unpacked_desc {
	u64 csb_ptr;
	u16 type;
	u8 subtype;
	bool vl;
	bool se;
	bool fe;
	bool ch;
	bool csr;
	bool rb;
	bool np;
	union {
		struct unpacked__copy copy;
		struct unpacked__repcopy repcopy;
		struct unpacked__intr intr;
		struct unpacked__sync sync;
		struct unpacked__cxt_start cxt_start;
		struct unpacked__cxt_stop cxt_stop;
		struct unpacked__cxt_upd cxt_upd;
	};
};

#define generic_field(_high, _low, _member)			\
	desc_field(_high, _low, struct unpacked_desc, _member)
#define generic_flag(_bit, _member) generic_field(_bit, _bit, _member)

static const struct packed_field_u16 generic_subfields[] = {
	generic_flag(0, vl),
	generic_flag(1, se),
	generic_flag(2, fe),
	generic_flag(3, ch),
	generic_flag(4, csr),
	generic_flag(5, rb),
	generic_field(15, 8, subtype),
	generic_field(26, 16, type),
	generic_flag(448, np),
	generic_field(511, 453, csb_ptr),
};

#ifndef SKIP_PACKING_CHECKS
#define define_unpack_fn(_T)						\
	static void unpack_ ## _T(struct unpacked_desc *to,		\
				  const struct sdxi_desc *from)		\
	{								\
		unpack_fields(from, sizeof(*from), to,	\
			      generic_subfields, SDXI_PACKING_QUIRKS);	\
		unpack_fields(from, sizeof(*from), &to->_T,		\
			      _T ## _subfields, SDXI_PACKING_QUIRKS);	\
	}
#else
#define define_unpack_fn(_T)						\
	static void unpack_ ## _T(struct unpacked_desc *to,		\
				  const struct sdxi_desc *from)		\
	{								\
		unpack_fields_u16(from, sizeof(*from), to,		\
				  generic_subfields,			\
				  ARRAY_SIZE(generic_subfields),	\
				  SDXI_PACKING_QUIRKS);			\
		unpack_fields_u16(from, sizeof(*from), &to->_T,		\
				  _T ## _subfields,			\
				  ARRAY_SIZE(_T ## _subfields),		\
				  SDXI_PACKING_QUIRKS);			\
	}
#endif	/* SKIP_PACKING_CHECKS */

define_unpack_fn(intr)
define_unpack_fn(copy)
define_unpack_fn(repcopy)
define_unpack_fn(sync)
define_unpack_fn(cxt_start)
define_unpack_fn(cxt_stop)
define_unpack_fn(cxt_upd)

static void desc_poison(struct sdxi_desc *d)
{
	memset(d, 0xff, sizeof(*d));
}

static void encode_size32(struct kunit *t)
{
	__le32 res = cpu_to_le32(U32_MAX);

	/* Valid sizes. */
	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_size32(1, &res));
	KUNIT_EXPECT_EQ(t, 0, le32_to_cpu(res));

	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_size32(SZ_4K, &res));
	KUNIT_EXPECT_EQ(t, SZ_4K - 1, le32_to_cpu(res));

	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_size32(SZ_4M, &res));
	KUNIT_EXPECT_EQ(t, SZ_4M - 1, le32_to_cpu(res));

	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_size32(SZ_4G - 1, &res));
	KUNIT_EXPECT_EQ(t, SZ_4G - 2, le32_to_cpu(res));

	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_size32(SZ_4G, &res));
	KUNIT_EXPECT_EQ(t, SZ_4G - 1, le32_to_cpu(res));

	/* Invalid sizes. Ensure the out parameter is unmodified. */
#define RES_VAL 0x843829
	res = cpu_to_le32(RES_VAL);

	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_size32(0, &res));
	KUNIT_EXPECT_EQ(t, RES_VAL, le32_to_cpu(res));

	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_size32(SZ_4G + 1, &res));
	KUNIT_EXPECT_EQ(t, RES_VAL, le32_to_cpu(res));

	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_size32(SZ_8G, &res));
	KUNIT_EXPECT_EQ(t, RES_VAL, le32_to_cpu(res));

	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_size32(U64_MAX, &res));
	KUNIT_EXPECT_EQ(t, RES_VAL, le32_to_cpu(res));

#undef RES_VAL
}

static void copy(struct kunit *t)
{
	struct unpacked_desc unpacked;
	struct sdxi_desc desc = {};
	struct sdxi_copy copy = {
		.src = 0x1000,
		.dst = 0x2000,
		.len = 4096,
		.src_akey = 0,
		.dst_akey = 0,
	};

	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_copy(&desc, &copy));

	unpack_copy(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.vl, 0);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_COPY);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_DMAB);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);

	KUNIT_EXPECT_EQ(t, unpacked.copy.size, copy.len - 1);

	/* Size boundaries. */
	desc_poison(&desc);
	copy.len = 0;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_copy(&desc, &copy));

	desc_poison(&desc);
	copy.len = 1;
	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_copy(&desc, &copy));
	unpack_copy(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.copy.size, copy.len - 1);

	/* SDXI forbids overlapping source and destination. */
	desc_poison(&desc);
	copy.len = 4097;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_copy(&desc, &copy));
	copy = (typeof(copy)) {
		.src = 0x4000,
		.dst = 0x4000,
		.len = 1,
		.src_akey = 0,
		.dst_akey = 0,
	};
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_copy(&desc, &copy));

	desc_poison(&desc);
	KUNIT_EXPECT_EQ(t, 0,
			sdxi_encode_copy(&desc,
					 &(struct sdxi_copy) {
						 .src = 0x1000,
						 .dst = 0x2000,
						 .len = 0x100,
						 .src_akey = 1,
						 .dst_akey = 2,
					 }));
	KUNIT_EXPECT_EQ(t, 0x1000, le64_to_cpu(desc.copy.addr0));
	KUNIT_EXPECT_EQ(t, 0x2000, le64_to_cpu(desc.copy.addr1));
	KUNIT_EXPECT_EQ(t, 0x100, 1 + le32_to_cpu(desc.copy.size));
	KUNIT_EXPECT_EQ(t, 1, le16_to_cpu(desc.copy.akey0));
	KUNIT_EXPECT_EQ(t, 2, le16_to_cpu(desc.copy.akey1));

	unpack_copy(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.vl, 0);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_COPY);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_DMAB);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);

	KUNIT_EXPECT_EQ(t, unpacked.copy.size, 0x100 - 1);
}

static void intr(struct kunit *t)
{
	struct unpacked_desc unpacked;
	struct sdxi_intr intr = {
		.akey = 1234,
	};
	struct sdxi_desc desc;

	desc_poison(&desc);
	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_intr(&desc, &intr));

	unpack_intr(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.vl, 0);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_INTR);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_INTR);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);

	KUNIT_EXPECT_EQ(t, unpacked.intr.akey, 1234);
}

static void cxt_start(struct kunit *t)
{
	struct unpacked_desc unpacked;
	struct sdxi_cxt_start start = {
		.range = sdxi_cxt_range_single(2),
	};
	struct sdxi_desc desc;

	desc_poison(&desc);
	KUNIT_ASSERT_EQ(t, 0, sdxi_encode_cxt_start(&desc, &start));

	unpack_cxt_start(&unpacked, &desc);

	KUNIT_EXPECT_EQ(t, 0, desc.cxt_start.vflags);

	KUNIT_EXPECT_EQ(t, unpacked.vl, 0);
	/* Context start requires FE=1 and SE=0. */
	KUNIT_EXPECT_EQ(t, unpacked.se, 0);
	KUNIT_EXPECT_EQ(t, unpacked.fe, 1);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_CXT_START_NM);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_ADMIN);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);

	KUNIT_EXPECT_FALSE(t, unpacked.cxt_start.dv);
	KUNIT_EXPECT_FALSE(t, unpacked.cxt_start.vf);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_start.cxt_start, 2);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_start.cxt_end, 2);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_start.vf_num, 0);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_start.db_value, 0);
}

static void cxt_stop(struct kunit *t)
{
	struct unpacked_desc unpacked;
	struct sdxi_cxt_stop stop = {
		.range = sdxi_cxt_range_single(2),
	};
	struct sdxi_desc desc;

	desc_poison(&desc);
	KUNIT_ASSERT_EQ(t, 0, sdxi_encode_cxt_stop(&desc, &stop));

	unpack_cxt_stop(&unpacked, &desc);

	KUNIT_EXPECT_EQ(t, 0, desc.cxt_stop.vflags);

	KUNIT_EXPECT_EQ(t, unpacked.vl, 0);
	KUNIT_EXPECT_EQ(t, unpacked.se, 0);
	KUNIT_EXPECT_EQ(t, unpacked.fe, 1);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_CXT_STOP);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_ADMIN);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);

	KUNIT_EXPECT_FALSE(t, unpacked.cxt_stop.hs);
	KUNIT_EXPECT_FALSE(t, unpacked.cxt_stop.vf);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_stop.cxt_start, 2);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_stop.cxt_end, 2);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_stop.vf_num, 0);
}

static void cxt_upd(struct kunit *t)
{
	struct unpacked_desc unpacked;
	struct sdxi_cxt_upd upd = {
		.level = SDXI_CXT_UPD_LVL_L2,
		.range = sdxi_cxt_range(128, 255),
	};
	struct sdxi_desc desc;

	desc_poison(&desc);
	KUNIT_ASSERT_EQ(t, 0, sdxi_encode_cxt_upd(&desc, &upd));

	unpack_cxt_upd(&unpacked, &desc);

	KUNIT_EXPECT_EQ(t, 0, desc.cxt_upd.vflags);

	KUNIT_EXPECT_EQ(t, unpacked.vl, 0);
	KUNIT_EXPECT_EQ(t, unpacked.se, 0);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_CXT_UPD);
	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_ADMIN);
	KUNIT_EXPECT_EQ(t, unpacked.csb_ptr, 0);
	KUNIT_EXPECT_EQ(t, unpacked.np, 1);

	KUNIT_EXPECT_EQ(t, unpacked.cxt_upd.pcd, SDXI_CXT_UPD_LVL_L2);
	KUNIT_EXPECT_FALSE(t, unpacked.cxt_upd.vf);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_upd.vf_num, 0);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_upd.cxt_start, 128);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_upd.cxt_end, 255);

	/* The other defined levels encode into the tgt_pcd subfield. */
	desc_poison(&desc);
	upd.level = SDXI_CXT_UPD_LVL_CTL;
	KUNIT_ASSERT_EQ(t, 0, sdxi_encode_cxt_upd(&desc, &upd));
	unpack_cxt_upd(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_upd.pcd, SDXI_CXT_UPD_LVL_CTL);

	desc_poison(&desc);
	upd.level = SDXI_CXT_UPD_LVL_L1;
	KUNIT_ASSERT_EQ(t, 0, sdxi_encode_cxt_upd(&desc, &upd));
	unpack_cxt_upd(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.cxt_upd.pcd, SDXI_CXT_UPD_LVL_L1);
}

static void sync(struct kunit *t)
{
	struct sdxi_sync sync = {
		.filter = SDXI_SYNC_FLT_STOP,
		.range = sdxi_cxt_range(1, U16_MAX),
	};
	struct sdxi_desc desc;
	struct unpacked_desc unpacked;

	desc_poison(&desc);
	KUNIT_ASSERT_EQ(t, 0, sdxi_encode_sync(&desc, &sync));
	unpack_sync(&unpacked, &desc);

	KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_ADMIN);
	KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_SYNC);
	KUNIT_EXPECT_EQ(t, unpacked.ch, 0);
	KUNIT_EXPECT_EQ(t, unpacked.sync.flt, SDXI_SYNC_FLT_STOP);
	KUNIT_EXPECT_EQ(t, unpacked.sync.cxt_start, 1);
	KUNIT_EXPECT_EQ(t, unpacked.sync.cxt_end, U16_MAX);
}

static void repcopy(struct kunit *t)
{
	static const u64 sizes[] = { SZ_4K, SZ_8K, SZ_64K, SZ_1M, SZ_2M };
	static const u64 counts[] = { 1, 2, 17, 512, BIT_ULL(20) };
	struct unpacked_desc unpacked;
	struct sdxi_desc desc;
	struct sdxi_repcopy rc;

	for (int i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (int j = 0; j < ARRAY_SIZE(counts); j++) {
			rc = (struct sdxi_repcopy) {
				.src = 0x100000,
				.dst = 0x20000000000,
				.src_len = sizes[i],
				.nr_copies = counts[j],
				.src_akey = 3,
				.dst_akey = 5,
			};
			desc_poison(&desc);
			KUNIT_ASSERT_EQ(t, 0, sdxi_encode_repcopy(&desc, &rc));
			unpack_repcopy(&unpacked, &desc);

			KUNIT_EXPECT_EQ(t, unpacked.vl, 0);
			KUNIT_EXPECT_EQ(t, unpacked.type, SDXI_DSC_OP_TYPE_DMAB);
			KUNIT_EXPECT_EQ(t, unpacked.subtype, SDXI_DSC_OP_SUBTYPE_REPCOPY);
			KUNIT_EXPECT_EQ(t, unpacked.np, 1);
			/* Both counts are encoded as value - 1, not shifted. */
			KUNIT_EXPECT_EQ(t, unpacked.repcopy.nsize, sizes[i] / SZ_4K - 1);
			KUNIT_EXPECT_EQ(t, unpacked.repcopy.num, counts[j] - 1);
			KUNIT_EXPECT_EQ(t, unpacked.repcopy.addr0, rc.src >> 12);
			KUNIT_EXPECT_EQ(t, unpacked.repcopy.addr1, rc.dst >> 12);
			KUNIT_EXPECT_EQ(t, unpacked.repcopy.akey0, 3);
			KUNIT_EXPECT_EQ(t, unpacked.repcopy.akey1, 5);
			KUNIT_EXPECT_EQ(t, unpacked.repcopy.az, 0);
		}
	}

	/* az is set only on request and does not disturb the address. */
	rc = (struct sdxi_repcopy) {
		.src = 0x7ffff000,
		.dst = 0x80000000,
		.src_len = SZ_4K,
		.nr_copies = 4,
		.zero = true,
	};
	KUNIT_ASSERT_EQ(t, 0, sdxi_encode_repcopy(&desc, &rc));
	unpack_repcopy(&unpacked, &desc);
	KUNIT_EXPECT_EQ(t, unpacked.repcopy.az, 1);
	KUNIT_EXPECT_EQ(t, unpacked.repcopy.addr0, 0x7ffff);
	KUNIT_EXPECT_EQ(t, unpacked.repcopy.addr1, 0x80000);
}

static void repcopy_invalid(struct kunit *t)
{
	struct sdxi_desc desc = {};
	struct sdxi_repcopy ok = {
		.src = 0x1000,
		.dst = 0x100000,
		.src_len = SZ_4K,
		.nr_copies = 1,
	};
	struct sdxi_repcopy rc;

	KUNIT_ASSERT_EQ(t, 0, sdxi_encode_repcopy(&desc, &ok));

	rc = ok;
	rc.src |= 0x10;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));

	rc = ok;
	rc.dst |= 0x800;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));

	rc = ok;
	rc.src_len = 0;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));

	rc = ok;
	rc.src_len = SZ_4K - 1;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));

	rc = ok;
	rc.src_len = SZ_2M + SZ_4K;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));

	rc = ok;
	rc.nr_copies = 0;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));

	rc = ok;
	rc.nr_copies = BIT_ULL(20) + 1;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));

	/* The last copy lands on the source. */
	rc = ok;
	rc.src = 0x3000;
	rc.dst = 0x1000;
	rc.nr_copies = 3;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));
	rc.nr_copies = 2;
	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_repcopy(&desc, &rc));

	/* A destination run that wraps past the top of the address space. */
	rc = ok;
	rc.dst = U64_MAX - SZ_4K + 1;
	rc.nr_copies = 2;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));
	rc.nr_copies = 1;
	KUNIT_EXPECT_EQ(t, 0, sdxi_encode_repcopy(&desc, &rc));

	/* A source that wraps. */
	rc = ok;
	rc.src = U64_MAX - SZ_4K + 1;
	rc.src_len = SZ_8K;
	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_encode_repcopy(&desc, &rc));
}

static void fill_plan(struct kunit *t)
{
	struct sdxi_fill_plan p;

	KUNIT_EXPECT_EQ(t, SZ_2M, sdxi_repcopy_max_dst(0));
	KUNIT_EXPECT_EQ(t, SZ_1G, sdxi_repcopy_max_dst(9));
	KUNIT_EXPECT_EQ(t, SZ_4G, sdxi_repcopy_max_dst(11));
	KUNIT_EXPECT_EQ(t, SZ_4G, sdxi_repcopy_max_dst(15));

	KUNIT_EXPECT_EQ(t, -EINVAL, sdxi_plan_fill(0x1000, 0, 11, &p));

	/* Aligned: body only. */
	KUNIT_ASSERT_EQ(t, 0, sdxi_plan_fill(SZ_2M, SZ_2M, 11, &p));
	KUNIT_EXPECT_EQ(t, p.head, 0);
	KUNIT_EXPECT_EQ(t, p.tail, 0);
	KUNIT_EXPECT_EQ(t, p.body, SZ_2M);
	KUNIT_EXPECT_EQ(t, p.nr_body, 1);

	/* Unaligned both ends. */
	KUNIT_ASSERT_EQ(t, 0, sdxi_plan_fill(SZ_4K + 100, SZ_2M, 11, &p));
	KUNIT_EXPECT_EQ(t, p.head, SZ_4K - 100);
	KUNIT_EXPECT_EQ(t, p.body, SZ_2M - SZ_4K);
	KUNIT_EXPECT_EQ(t, p.tail, 100);
	KUNIT_EXPECT_EQ(t, p.head + p.body + p.tail, SZ_2M);

	/* Entirely inside one page, and a head that ends the fill. */
	KUNIT_ASSERT_EQ(t, 0, sdxi_plan_fill(0x1010, 32, 11, &p));
	KUNIT_EXPECT_EQ(t, p.head, 32);
	KUNIT_EXPECT_EQ(t, p.nr_body, 0);
	KUNIT_EXPECT_EQ(t, p.tail, 0);
	KUNIT_ASSERT_EQ(t, 0, sdxi_plan_fill(0x1ff0, 16, 11, &p));
	KUNIT_EXPECT_EQ(t, p.head, 16);
	KUNIT_EXPECT_EQ(t, p.nr_body + p.tail, 0);

	/* Splitting on the chunk size. */
	KUNIT_ASSERT_EQ(t, 0, sdxi_plan_fill(0, 16ULL * SZ_1G, 11, &p));
	KUNIT_EXPECT_EQ(t, p.nr_body, 4);
	KUNIT_ASSERT_EQ(t, 0, sdxi_plan_fill(0, 4ULL * SZ_1G + SZ_4K, 11, &p));
	KUNIT_EXPECT_EQ(t, p.nr_body, 2);
	KUNIT_ASSERT_EQ(t, 0, sdxi_plan_fill(0, 16ULL * SZ_1G, 0, &p));
	KUNIT_EXPECT_EQ(t, p.nr_body, 8192);
}

static struct kunit_case generic_desc_tcs[] = {
	KUNIT_CASE(encode_size32),
	KUNIT_CASE(copy),
	KUNIT_CASE(repcopy),
	KUNIT_CASE(repcopy_invalid),
	KUNIT_CASE(fill_plan),
	KUNIT_CASE(intr),
	KUNIT_CASE(cxt_start),
	KUNIT_CASE(cxt_stop),
	KUNIT_CASE(cxt_upd),
	KUNIT_CASE(sync),
	{}
};

static int generic_desc_setup_device(struct kunit *t)
{
	struct device *dev = kunit_device_register(t, "sdxi-mock-device");

	KUNIT_ASSERT_NOT_ERR_OR_NULL(t, dev);
	t->priv = dev;
	return 0;
}

static struct kunit_suite generic_desc_ts = {
	.name = "Generic SDXI descriptor encoding",
	.test_cases = generic_desc_tcs,
	.init = generic_desc_setup_device,
};
kunit_test_suite(generic_desc_ts);

MODULE_DESCRIPTION("SDXI descriptor encoding tests");
MODULE_AUTHOR("Nathan Lynch");
MODULE_LICENSE("GPL");
