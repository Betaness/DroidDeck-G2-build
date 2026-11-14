// LD_PRELOAD shim for the local lavapipe run only: shows /proc/cpuinfo with every "CPU part" as 0xd0b (Cortex-A76).
// On CPUs whose kernel does not enable SVE for user space (Qualcomm phones: the K70's Cortex-X4/A720/A520) LLVM
// still picks the core by its part number and llvmpipe's JIT emits SVE (addvl), which dies with SIGILL there.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fake_cpuinfo(void) {
  int fd = memfd_create("cpuinfo", 0);
  if (fd < 0) return -1;
  FILE *in = fopen("/proc/cpuinfo", "r");
  if (!in) { close(fd); return -1; }
  char line[512];
  while (fgets(line, sizeof line, in)) {
    if (!strncmp(line, "CPU part", 8)) snprintf(line, sizeof line, "CPU part\t: 0xd0b\n");
    if (write(fd, line, strlen(line)) < 0) break;
  }
  fclose(in);
  lseek(fd, 0, SEEK_SET);
  return fd;
}

#define WRAP(name, ...)                                                                    \
  int name(const char *path, int flags, ...) {                                             \
    static int (*real)(const char *, int, ...);                                            \
    if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, #name);            \
    if (path && !strcmp(path, "/proc/cpuinfo") && !(flags & (O_WRONLY | O_RDWR))) {        \
      int fd = fake_cpuinfo();                                                             \
      if (fd >= 0) return fd;                                                              \
    }                                                                                      \
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);               \
    return real(path, flags, mode);                                                        \
  }
WRAP(open)
WRAP(open64)

int openat(int dir, const char *path, int flags, ...) {
  static int (*real)(int, const char *, int, ...);
  if (!real) real = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat");
  if (path && !strcmp(path, "/proc/cpuinfo") && !(flags & (O_WRONLY | O_RDWR))) {
    int fd = fake_cpuinfo();
    if (fd >= 0) return fd;
  }
  va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
  return real(dir, path, flags, mode);
}
