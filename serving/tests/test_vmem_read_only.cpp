#define main search_beam_program_main
#include "../../serving/search_beam.cpp"
#undef main

#include <cassert>
#include <csignal>
#include <cstdio>
#include <sys/wait.h>

int main() {
  char path[] = "/tmp/flashanns-vmem-ro-XXXXXX";
  const int seed_fd = mkstemp(path);
  assert(seed_fd >= 0);
  assert(ftruncate(seed_fd, 4096) == 0);
  close(seed_fd);

  int mapped_fd = -1;
  void* mapping = map_vmem_ro(path, 0, 4096, &mapped_fd);
  assert(mapping != MAP_FAILED);
  assert(mapped_fd >= 0);

  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    static_cast<volatile unsigned char*>(mapping)[0] = 0x5a;
    _exit(0);
  }

  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status));
  assert(WTERMSIG(status) == SIGSEGV);

  assert(munmap(mapping, 4096) == 0);
  close(mapped_fd);
  assert(unlink(path) == 0);
  std::puts("test_vmem_read_only OK");
  return 0;
}
