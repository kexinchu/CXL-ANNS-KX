#pragma once
// Issue VMEM_IOC_PREFETCH_BATCH so NAND fills run concurrently before memcpy.
// Offsets are absolute logical bytes on /dev/vmem0.

#include <cstdint>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

struct VmemIo {
  int fd = -1;
  off_t image_off = 0;
  bool prefetch = true;
};

#ifndef VMEM_IOC_MAGIC
#define VMEM_IOC_MAGIC 'V'
#define VMEM_BATCH_MAX 64U
struct vmem_batch_prefetch {
  uint32_t count;
  uint32_t _pad;
  uint64_t offsets[VMEM_BATCH_MAX];
};
#define VMEM_IOC_PREFETCH_BATCH _IOW(VMEM_IOC_MAGIC, 9, struct vmem_batch_prefetch)
#endif

inline int vmem_prefetch_pages(const VmemIo& io, const uint64_t* page_offs, int n,
                               size_t page_bytes) {
  if (!io.prefetch || io.fd < 0 || n <= 0) return 0;
  int issued = 0;
  vmem_batch_prefetch bp;
  int i = 0;
  while (i < n) {
    memset(&bp, 0, sizeof(bp));
    unsigned c = 0;
    while (i < n && c < VMEM_BATCH_MAX) {
      bp.offsets[c++] = (uint64_t)io.image_off + page_offs[i++];
    }
    bp.count = c;
    if (ioctl(io.fd, VMEM_IOC_PREFETCH_BATCH, &bp) != 0) return -1;
    issued += (int)c;
    (void)page_bytes;
  }
  return issued;
}
