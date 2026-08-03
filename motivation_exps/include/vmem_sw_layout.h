/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NVMEX_VMEM_SW_LAYOUT_H
#define _NVMEX_VMEM_SW_LAYOUT_H

#include <linux/types.h>

enum vmem_sw_tier {
	VMEM_SW_TIER_RAM = 0,
	VMEM_SW_TIER_SSD = 1,
};

struct vmem_sw_layout {
	__u64 ram_size;
	__u64 ssd_size;
	__u64 logical_size;
	__u64 stripe_size;
	__u64 ram_stripes;
	__u64 ssd_stripes;
	__u64 full_stripes;
	__u64 full_span;
	__u64 ssd_tail;
};

struct vmem_sw_mapping {
	__u64 tier_offset;
	__u64 contiguous;
	__u32 tier;
	__u32 _reserved;
};

static inline int
vmem_sw_map_offset(const struct vmem_sw_layout *l, __u64 logical,
		   struct vmem_sw_mapping *m)
{
	__u64 stripe;
	__u64 in_stripe;
	__u64 ram_before;
	__u64 ram_after;

	if (!l || !m || !l->stripe_size || logical >= l->logical_size)
		return -1;

	if (logical >= l->full_span) {
		m->tier = VMEM_SW_TIER_SSD;
		m->tier_offset = l->ssd_stripes * l->stripe_size +
				 (logical - l->full_span);
		m->contiguous = l->logical_size - logical;
		m->_reserved = 0;
		return 0;
	}

	stripe = logical / l->stripe_size;
	in_stripe = logical % l->stripe_size;
	ram_before = stripe * l->ram_stripes / l->full_stripes;
	ram_after = (stripe + 1) * l->ram_stripes / l->full_stripes;

	if (ram_after != ram_before) {
		m->tier = VMEM_SW_TIER_RAM;
		m->tier_offset = ram_before * l->stripe_size + in_stripe;
	} else {
		m->tier = VMEM_SW_TIER_SSD;
		m->tier_offset = (stripe - ram_before) * l->stripe_size +
				 in_stripe;
	}
	m->contiguous = l->stripe_size - in_stripe;
	m->_reserved = 0;
	return 0;
}

#endif
