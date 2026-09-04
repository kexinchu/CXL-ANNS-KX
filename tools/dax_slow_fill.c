// Probe: does /dev/dax0.0 keep data if we write slowly?
// Maps one 2MiB window at a time (dax align). Optional sleep after each window.
#define _GNU_SOURCE
#include <emmintrin.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define WIN (2ull << 20)

static void nap_us(unsigned us) {
  if (!us) return;
  struct timespec ts = {.tv_sec = us / 1000000u, .tv_nsec = (long)(us % 1000000u) * 1000L};
  nanosleep(&ts, NULL);
}

static int read_pkg_mc(void) {
  FILE* f = fopen("/sys/class/hwmon/hwmon6/temp1_input", "r");
  if (!f) return -1;
  int v = -1;
  if (fscanf(f, "%d", &v) != 1) v = -1;
  fclose(f);
  return v;
}

static void* map_win(int fd, off_t off, int wr) {
  int prot = PROT_READ | (wr ? PROT_WRITE : 0);
  void* p = mmap(NULL, WIN, prot, MAP_SHARED, fd, off);
  if (p == MAP_FAILED) {
    fprintf(stderr, "mmap off=%llx: %s\n", (unsigned long long)off, strerror(errno));
    return NULL;
  }
  return p;
}

int main(int argc, char** argv) {
  const char* dev = "/dev/dax0.0";
  size_t total = 1ull << 30;  // default 1 GiB
  unsigned gap_us = 10000;    // 10 ms / 2MiB ≈ 200 MiB/s
  unsigned fence_4k = 1;
  int verify_every = 16;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--bytes") && i + 1 < argc)
      total = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "--gap-us") && i + 1 < argc)
      gap_us = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--no-fence"))
      fence_4k = 0;
    else if (!strcmp(argv[i], "--verify-every") && i + 1 < argc)
      verify_every = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--dev") && i + 1 < argc)
      dev = argv[++i];
    else {
      fprintf(stderr,
              "usage: dax_slow_fill [--dev /dev/dax0.0] [--bytes N] [--gap-us N] "
              "[--no-fence] [--verify-every N]\n");
      return 2;
    }
  }
  total = (total + WIN - 1) & ~(WIN - 1);
  int fd = open(dev, O_RDWR);
  if (fd < 0) {
    perror("open dax");
    return 1;
  }
  const int nwin = (int)(total / WIN);
  fprintf(stderr, "fill %s bytes=%zu windows=%d gap_us=%u fence4k=%u pkg_mc=%d\n", dev,
          total, nwin, gap_us, fence_4k, read_pkg_mc());
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int bad = 0;
  for (int w = 0; w < nwin; ++w) {
    off_t off = (off_t)w * (off_t)WIN;
    void* p = map_win(fd, off, 1);
    if (!p) return 1;
    uint64_t tag = 0xC0DE0000ull + (uint64_t)w;
    volatile uint64_t* d = p;
    for (size_t i = 0; i < WIN / 8; ++i) {
      d[i] = tag;
      if (fence_4k && ((i + 1) & 511) == 0) _mm_sfence();  // every 4KiB
    }
    _mm_sfence();
    uint64_t got = d[0], tail = d[WIN / 8 - 1];
    munmap(p, WIN);
    void* q = map_win(fd, off, 0);
    if (!q) return 1;
    uint64_t r0 = ((const uint64_t*)q)[0];
    uint64_t r1 = ((const uint64_t*)q)[WIN / 8 - 1];
    munmap(q, WIN);
    if (got != tag || tail != tag || r0 != tag || r1 != tag) {
      fprintf(stderr, "FAIL win=%d same=%llx/%llx remap=%llx/%llx pkg_mc=%d\n", w,
              (unsigned long long)got, (unsigned long long)tail, (unsigned long long)r0,
              (unsigned long long)r1, read_pkg_mc());
      bad++;
      if (bad >= 3) break;
    }
    if (verify_every > 0 && w > 0 && (w % verify_every) == 0) {
      void* h = map_win(fd, 0, 0);
      if (!h) return 1;
      uint64_t h0 = ((const uint64_t*)h)[0];
      munmap(h, WIN);
      if (h0 != 0xC0DE0000ull) {
        fprintf(stderr, "FAIL head-clobber at win=%d head=%llx pkg_mc=%d\n", w,
                (unsigned long long)h0, read_pkg_mc());
        bad++;
        break;
      }
    }
    if ((w & 31) == 31) {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
      double mb = (w + 1) * (WIN / 1048576.0);
      fprintf(stderr, "  %6.0f MiB  %.1f MiB/s  pkg_mc=%d\n", mb, sec > 0 ? mb / sec : 0,
              read_pkg_mc());
    }
    nap_us(gap_us);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  fprintf(stderr, "done bad=%d sec=%.2f pkg_mc=%d\n", bad, sec, read_pkg_mc());
  close(fd);
  return bad ? 1 : 0;
}
