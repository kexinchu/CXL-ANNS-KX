// Pack pagebin vectors + raw graph into DiskANN fixed-stride records.
// Writes version=2 CxanLayoutHeader + n * STRIDE. Can target a file or /dev/dax*.
#include "serving/placement.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

// CXL DAX drops glibc NT memcpy; ordinary stores + sfence.
static void dax_copy(void* dst, const void* src, size_t n) {
  auto* d = static_cast<volatile uint8_t*>(dst);
  const auto* s = static_cast<const uint8_t*>(src);
  size_t i = 0;
  if (((uintptr_t)dst & 7) == 0 && ((uintptr_t)src & 7) == 0) {
    auto* dq = reinterpret_cast<volatile uint64_t*>(dst);
    const auto* sq = reinterpret_cast<const uint64_t*>(src);
    for (; i + 8 <= n; i += 8) dq[i / 8] = sq[i / 8];
  }
  for (; i < n; ++i) d[i] = s[i];
}

static void die(const char* m) {
  perror(m);
  std::exit(1);
}

int main(int argc, char** argv) {
  const char* vecs = nullptr;
  const char* graph = nullptr;
  const char* out = nullptr;
  size_t stride = 2048;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--vecs") vecs = argv[++i];
    else if (a == "--graph") graph = argv[++i];
    else if (a == "--out") out = argv[++i];
    else if (a == "--stride") stride = (size_t)strtoull(argv[++i], nullptr, 10);
    else {
      fprintf(stderr, "unknown %s\n", a.c_str());
      return 2;
    }
  }
  if (!vecs || !graph || !out) {
    fprintf(stderr, "usage: pack_diskann_entry --vecs pagebin_image.bin --graph pagebin_graph.bin "
                    "--out diskann.bin|--out /dev/dax0.0 [--stride 2048]\n");
    return 2;
  }

  int vf = open(vecs, O_RDONLY);
  if (vf < 0) die("open vecs");
  struct stat st{};
  if (fstat(vf, &st) != 0) die("stat vecs");
  void* vm = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, vf, 0);
  if (vm == MAP_FAILED) die("mmap vecs");
  auto* hdr_in = reinterpret_cast<const CxanLayoutHeader*>(vm);
  if (hdr_in->magic != kCxanMagic) {
    fprintf(stderr, "bad vecs magic\n");
    return 1;
  }
  const uint32_t n = hdr_in->n, dim = hdr_in->dim, R = hdr_in->R, vb = hdr_in->vec_bytes;
  const size_t packed = (size_t)dim * vb;
  const size_t need = Placement::diskann_payload_bytes(dim, vb, R);
  stride = Placement::diskann_stride_for(dim, vb, R, stride);
  if (stride < need) {
    fprintf(stderr, "stride %zu < payload %zu\n", stride, need);
    return 1;
  }
  const uint8_t* vbase = (const uint8_t*)vm + hdr_in->off_vectors;
  const size_t vstride = (size_t)(hdr_in->len_vectors / n);

  int gf = open(graph, O_RDONLY);
  if (gf < 0) die("open graph");
  struct stat gs{};
  if (fstat(gf, &gs) != 0) die("stat graph");
  if ((size_t)gs.st_size < (size_t)n * R * 4) {
    fprintf(stderr, "graph too small\n");
    return 1;
  }
  void* gm = mmap(nullptr, (size_t)gs.st_size, PROT_READ, MAP_PRIVATE, gf, 0);
  if (gm == MAP_FAILED) die("mmap graph");
  const uint32_t* gbase = reinterpret_cast<const uint32_t*>(gm);

  const size_t out_bytes = 4096 + (size_t)n * stride;
  int of = open(out, O_RDWR | O_CREAT, 0644);
  if (of < 0) die("open out");
  struct stat os{};
  if (fstat(of, &os) != 0) die("stat out");
  const bool is_dev = S_ISCHR(os.st_mode) || S_ISBLK(os.st_mode);
  if (!is_dev) {
    if (ftruncate(of, (off_t)out_bytes) != 0) die("ftruncate out");
  } else if ((size_t)os.st_size != 0 && (size_t)os.st_size < out_bytes) {
    fprintf(stderr, "device %s size %zu < need %zu\n", out, (size_t)os.st_size, out_bytes);
    return 1;
  }
  size_t map_len = out_bytes;
  if (is_dev) {
    const size_t two_m = 2ull << 20;
    map_len = (out_bytes + two_m - 1) & ~(two_m - 1);
  }
  void* om = mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, of, 0);
  if (om == MAP_FAILED) die("mmap out");

  CxanLayoutHeader ho{};
  ho.magic = kCxanMagic;
  ho.version = 2;
  ho.n = n;
  ho.dim = dim;
  ho.R = R;
  ho.vec_bytes = vb;
  ho.entry_id = hdr_in->entry_id;
  ho.entry_nodes = hdr_in->entry_nodes;
  ho.off_vectors = 4096;
  ho.len_vectors = (uint64_t)n * stride;
  ho.off_graph = 0;
  ho.len_graph = 0;
  if (is_dev) {
    dax_copy(om, &ho, sizeof(ho));
    _mm_sfence();
  } else {
    std::memcpy(om, &ho, sizeof(ho));
  }

  auto* dst = static_cast<uint8_t*>(om) + 4096;
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* e = dst + (size_t)i * stride;
    if (is_dev) {
      dax_copy(e, vbase + (size_t)i * vstride, packed);
      uint32_t nn = R;
      dax_copy(e + packed, &nn, 4);
      dax_copy(e + packed + 4, gbase + (size_t)i * R, (size_t)R * 4);
      if ((i & 1023) == 1023) _mm_sfence();
    } else {
      std::memcpy(e, vbase + (size_t)i * vstride, packed);
      *reinterpret_cast<uint32_t*>(e + packed) = R;
      std::memcpy(e + packed + 4, gbase + (size_t)i * R, (size_t)R * 4);
    }
    if ((i & 0xfffff) == 0) {
      fprintf(stderr, "pack %u / %u\n", i, n);
      fflush(stderr);
    }
  }
  if (is_dev) _mm_sfence();
  volatile uint64_t magic_chk = *reinterpret_cast<volatile uint64_t*>(om);
  if (magic_chk != kCxanMagic) {
    fprintf(stderr, "post-write magic mismatch %llx\n", (unsigned long long)magic_chk);
    return 1;
  }
  munmap(om, map_len);
  close(of);
  // Re-open and confirm persistence (dax msync is EINVAL; remap is the check).
  if (is_dev) {
    int rf = open(out, O_RDONLY);
    if (rf < 0) die("reopen out");
    void* rm = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, rf, 0);
    if (rm == MAP_FAILED) die("remap out");
    auto* rh = reinterpret_cast<const CxanLayoutHeader*>(rm);
    if (rh->magic != kCxanMagic || rh->n != n) {
      fprintf(stderr, "remap magic/n fail magic=%llx n=%u\n",
              (unsigned long long)rh->magic, rh->n);
      return 1;
    }
    munmap(rm, map_len);
    close(rf);
  }
  munmap(vm, (size_t)st.st_size);
  munmap(gm, (size_t)gs.st_size);
  close(vf);
  close(gf);
  fprintf(stderr, "OK n=%u stride=%zu bytes=%zu map=%zu out=%s\n", n, stride, out_bytes,
          map_len, out);
  return 0;
}
