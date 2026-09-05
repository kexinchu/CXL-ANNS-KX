#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#define WIN (2ull << 20)
int main(int argc, char** argv) {
  size_t bytes = argc > 1 ? strtoull(argv[1], NULL, 10) : (512ull << 20);
  int fd = open("/dev/dax0.0", O_RDONLY);
  int nwin = (int)(bytes / WIN);
  int bad = 0;
  for (int w = 0; w < nwin; ++w) {
    void* p = mmap(NULL, WIN, PROT_READ, MAP_SHARED, fd, (off_t)w * WIN);
    if (p == MAP_FAILED) {
      perror("mmap");
      return 1;
    }
    uint64_t want = 0xC0DE0000ull + (uint64_t)w;
    uint64_t a = ((const uint64_t*)p)[0];
    uint64_t b = ((const uint64_t*)p)[WIN / 8 - 1];
    munmap(p, WIN);
    if (a != want || b != want) {
      if (bad < 8)
        printf("mismatch w=%d got %llx/%llx want %llx\n", w, (unsigned long long)a,
               (unsigned long long)b, (unsigned long long)want);
      bad++;
    }
  }
  printf("check windows=%d bad=%d\n", nwin, bad);
  close(fd);
  return bad ? 1 : 0;
}
