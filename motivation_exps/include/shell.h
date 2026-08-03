#ifndef _NVME_MEM2NVM_SHELL_H
#define _NVME_MEM2NVM_SHELL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#ifdef __KERNEL__
struct device;
#endif

enum hps_opcode {
	HPS_NOP = 0,
	HPS_COMPRESS,
	HPS_DECOMPRESS,
	HPS_FILTER,
	HPS_SCAN,
	HPS_ENCRYPT,
	HPS_DECRYPT,
	HPS_HASH,
	HPS_PAGE_FETCH,		/* SSD offset → BAR addr, len */
	HPS_PAGE_WRITEBACK,	/* BAR addr → SSD offset, len */
	HPS_PREFETCH_HINT,	/* host → HPS: suggest prefetch addr, len */
};

enum vmem_prefetch_policy {
	VMEM_PREFETCH_OFF        = 0,
	VMEM_PREFETCH_SEQUENTIAL = 1,
	VMEM_PREFETCH_STRIDE     = 2,
	VMEM_PREFETCH_ADAPTIVE   = 3,
};

enum vmem_mode_id {
	VMEM_MODE_DAX = 0,
	VMEM_MODE_KMEM,
	VMEM_MODE_OFF,
	VMEM_MODE_SOFTWARE,
};

#define VMEM_NUMA_NODE_NONE	((__u32)-1)
#define VMEM_IOC_MAGIC		'V'
#define VMEM_INFO_F_CXL_WINDOW	(1U << 0)
#define VMEM_INFO_F_SOFTWARE	(1U << 1)
#define VMEM_INFO_F_NVMEX_BAR	(1U << 2)

#ifdef __KERNEL__
struct vmem_bar_info {
	struct device *dev;
	__u64 phys;
	__u64 size;
	__s32 numa_node;
	__u32 flags;
};
#endif

struct vmem_hps_cmd {
	__u32 opcode;
	__u64 src_offset;
	__u64 dst_offset;
	__u64 length;
	__u64 result;
};

struct vmem_info {
	__u64 bar_phys;
	__u64 bar_size;
	__u64 ssd_size;
	__u32 mode;
	__u32 numa_node;
	__u32 flags;
	__u32 _reserved;
};

struct vmem_prefetch_cfg {
	__u32 policy;		/* enum vmem_prefetch_policy */
	__u32 depth;		/* pages ahead to prefetch */
};

struct vmem_prefetch_hint {
	__u64 ssd_offset;
	__u64 length;
};

#define VMEM_LAYOUT_ABI_VERSION	1

struct vmem_layout_info {
	__u32 abi_version;
	__u32 struct_size;
	__u64 logical_size;
	__u64 ram_size;
	__u64 ssd_size;
	__u64 stripe_size;
	__u64 cache_limit;
	__u64 cache_used;
	__u64 dirty_bytes;
	__u32 flags;
	__u32 _reserved;
};

struct vmem_logical_range {
	__u64 logical_offset;
	__u64 length;
};

#define VMEM_IOC_HPS_SUBMIT	_IOWR(VMEM_IOC_MAGIC, 1, struct vmem_hps_cmd)
#define VMEM_IOC_GET_INFO	_IOR(VMEM_IOC_MAGIC, 2, struct vmem_info)
#define VMEM_IOC_SET_PREFETCH	_IOW(VMEM_IOC_MAGIC, 3, struct vmem_prefetch_cfg)
#define VMEM_IOC_GET_PREFETCH	_IOR(VMEM_IOC_MAGIC, 4, struct vmem_prefetch_cfg)
#define VMEM_IOC_PREFETCH_HINT	_IOW(VMEM_IOC_MAGIC, 5, struct vmem_prefetch_hint)
#define VMEM_IOC_GET_LAYOUT	_IOR(VMEM_IOC_MAGIC, 6, struct vmem_layout_info)
#define VMEM_IOC_FLUSH		_IO(VMEM_IOC_MAGIC, 7)
#define VMEM_IOC_PREFETCH_LOGICAL \
	_IOW(VMEM_IOC_MAGIC, 8, struct vmem_logical_range)

#ifdef __KERNEL__
extern int shell_driver_get_queue_addr_ntc(int nvme_index, u64 *addr, int sqcq);
extern int shell_driver_set_db_reg_addr(u32 nvme_index, u32 qid, u64 sq, u64 cq);
extern int shell_driver_enable_sq(u32 nvme_index, u32 en_mask);
extern int shell_driver_hps_submit(u32 opcode, u64 src, u64 dst, u64 len,
				   u64 *result);
extern int shell_driver_hps_status(u32 *status);
extern u64 shell_driver_read_ssd_size(void);
extern int shell_driver_set_prefetch_policy(u32 policy, u32 depth);
extern int shell_driver_get_prefetch_policy(u32 *policy, u32 *depth);
extern int shell_driver_write_prefetch_hint(u64 addr);
extern int shell_driver_get_vmem_bar(struct vmem_bar_info *info);
extern void shell_driver_put_vmem_bar(struct device *dev);
#endif

#endif /* _NVME_MEM2NVM_SHELL_H */
