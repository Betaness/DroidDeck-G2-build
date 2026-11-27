import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

FASTPATH = Path(__file__).resolve().parents[1] / "proot/fastpath/fastpath.c"

# Drives the fast path with a stand-in trampoline. "trap NAME" answers that call with ENOSYS whatever its
# arguments, as proot does for a trampoline call a guest's seccomp filter traps; "nosys NAME" answers it with
# ENOSYS unless it carries the invalid arguments of the fast path's probe, as a filesystem without the call does;
# "nosys-fd NAME" does the same only for calls on a descriptor (an empty path), "nosys none" ends both.
# Exported wrappers print "libc" when they handed the call to libc.
PROGRAM = r"""
#include "FASTPATH"

static long trapped_nr = -1, unsupported_nr = -1, unsupported_fd_nr = -1;

static long host_sys(long nr, long a, long b, long c, long d, long e, long f) {
  if (nr == trapped_nr || (nr == unsupported_nr && a != -1)) return -ENOSYS;
  if (nr == unsupported_fd_nr && b != -1 && b != 0 && *(const char *)b == 0) return -ENOSYS;
  long r = syscall(nr, a, b, c, d, e, f);
  return r < 0 ? -errno : r;
}

static long number(const char *name) {
  if (!strcmp(name, "symlinkat")) return SYS_symlinkat;
  if (!strcmp(name, "readlinkat")) return SYS_readlinkat;
  if (!strcmp(name, "mkdirat")) return SYS_mkdirat;
  if (!strcmp(name, "openat")) return SYS_openat;
  if (!strcmp(name, "newfstatat")) return SYS_newfstatat;
  if (!strcmp(name, "fcntl")) return SYS_fcntl;
  if (!strcmp(name, "none")) return -1;
  exit(5);
}

static unsigned long misses_before;
static void begin(void) { misses_before = misses; }
static void done(int ok, int err) {
  if (misses != misses_before) puts("libc");
  else if (ok) puts("ok");
  else printf("E%d\n", err);
}

static int unix_socket(void) { return (int)syscall(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); }
static socklen_t unix_address(struct sockaddr_un *un, const char *path) {
  memset(un, 0, sizeof *un);
  un->sun_family = AF_UNIX;
  snprintf(un->sun_path, sizeof un->sun_path, "%s", path);
  return offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
}

// Counted with raw calls: the fast path answers opendir() itself, for guest paths.
static int open_fds(void) {
  int n = 0;
  for (long fd = 0; fd < 4096; fd++) n += syscall(SYS_fcntl, fd, F_GETFD) >= 0;
  return n;
}

static void show(long r) {
  if (refused()) puts("REFUSED");
  else if (r == FP_SLOW) puts("SLOW");
  else if (r < 0) printf("E%ld\n", -r);
  else puts("ok");
}

static int listener = -1;

int main(int argc, char **argv) {
  fp_sys = host_sys;
  snprintf(root, sizeof root, "%s", argv[1]);
  rootlen = strlen(root);
  pthread_once(&init_once, fp_init);
  fp_on = 1;
  int base = open_fds();
  for (int i = 2; i < argc; i++) {
    const char *op = argv[i];
    if (!strcmp(op, "trap")) trapped_nr = number(argv[++i]);
    else if (!strcmp(op, "nosys")) unsupported_nr = unsupported_fd_nr = number(argv[++i]);
    else if (!strcmp(op, "nosys-fd")) unsupported_fd_nr = number(argv[++i]);
    else if (!strcmp(op, "flag")) printf("%d\n", fp_refused);
    else if (!strcmp(op, "opendir")) {
      begin();
      DIR *d = opendir(argv[++i]);
      int err = errno;
      done(d != NULL, err);
      if (d) closedir(d);
    } else if (!strcmp(op, "fopen")) {
      begin();
      FILE *f = fopen(argv[++i], "r");
      int err = errno;
      done(f != NULL, err);
      if (f) fclose(f);
    } else if (!strcmp(op, "realpath")) {
      begin();
      char *p = realpath(argv[++i], NULL);
      int err = errno;
      done(p != NULL, err);
      free(p);
    } else if (!strcmp(op, "getcwd")) {
      char buf[PATH_MAX];
      begin();
      char *p = getcwd(buf, sizeof buf);
      done(p != NULL, errno);
    } else if (!strcmp(op, "nprocs")) {
      begin();
      done(get_nprocs() > 0, 0);
    } else if (!strcmp(op, "getpwuid")) {
      begin();
      done(getpwuid(0) != NULL, errno);
    } else if (!strcmp(op, "listen")) {
      // A listening socket at guest path P (bound through the fast path) with two connections waiting.
      const char *path = argv[++i];
      struct sockaddr_un un;
      socklen_t len = unix_address(&un, path);
      listener = unix_socket();
      if (bind(listener, (struct sockaddr *)&un, len) != 0 || syscall(SYS_listen, listener, 4) != 0) return 3;
      for (int n = 0; n < 2; n++)
        if (connect(unix_socket(), (struct sockaddr *)&un, len) != 0) return 4;
      puts("listening");
    } else if (!strcmp(op, "accept")) {
      begin();
      int fd = accept(listener, NULL, NULL);
      done(fd >= 0, errno);
      if (fd >= 0) close(fd);
    } else if (!strcmp(op, "getsockname")) {
      struct sockaddr_un un;
      socklen_t len = sizeof un;
      begin();
      int r = getsockname(listener, (struct sockaddr *)&un, &len);
      int err = errno;
      if (r == 0 && misses == misses_before) puts(un.sun_path);
      else done(0, err);
    } else if (!strcmp(op, "connect")) {
      struct sockaddr_un un;
      socklen_t len = unix_address(&un, argv[++i]);
      int fd = unix_socket();
      begin();
      int r = connect(fd, (struct sockaddr *)&un, len);
      done(r == 0, errno);
      close(fd);
    } else if (!strcmp(op, "symlink")) show(fp_symlink("target", AT_FDCWD, argv[++i]));
    else if (!strcmp(op, "mkdir")) show(fp_mkdir(AT_FDCWD, argv[++i], 0755));
    else if (!strcmp(op, "stat")) {
      struct stat st;
      show(fp_stat(AT_FDCWD, argv[++i], AT_SYMLINK_NOFOLLOW, &st));
    } else if (!strcmp(op, "canon")) {
      char path[PATH_MAX];
      snprintf(path, sizeof path, "%s", argv[++i]);
      show(canonical(path, sizeof path));
    } else if (!strcmp(op, "on")) printf("%d\n", __atomic_load_n(&fp_on, __ATOMIC_RELAXED));
    else if (!strcmp(op, "fds")) printf("%d\n", open_fds() - base);
    else return 2;
    fflush(stdout);
  }
  return 0;
}
"""


class FastPathEnosysTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("gcc") is None:
            raise unittest.SkipTest("gcc is not available")
        cls.work = Path(tempfile.mkdtemp())
        source = cls.work / "enosys.c"
        source.write_text(PROGRAM.replace("FASTPATH", str(FASTPATH)))
        cls.program = cls.work / "enosys"
        subprocess.run(["gcc", "-O1", "-w", "-pthread", "-o", str(cls.program), str(source), "-ldl"], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(dir=self.work))
        (self.root / "d").mkdir()
        (self.root / "d/file").touch()
        (self.root / "sys/devices/system/cpu").mkdir(parents=True)
        (self.root / "sys/devices/system/cpu/online").write_text("0-3\n")
        (self.root / "etc").mkdir()
        (self.root / "etc/nsswitch.conf").write_text("passwd: files\n")
        (self.root / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\n")

    def ask(self, *ops):
        result = subprocess.run([str(self.program), str(self.root), *ops], capture_output=True, text=True, timeout=60,
                                cwd=self.root / "d")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        return result.stdout.split()

    def test_a_call_the_filesystem_does_not_implement_leaves_the_fast_path_on(self):
        out = self.ask("nosys", "symlinkat", "symlink", "/d/link", "on", "stat", "/d/file", "symlink", "/d/link", "on",
                       "mkdir", "/d/new", "on")
        self.assertEqual(["REFUSED", "1", "ok", "REFUSED", "1", "ok", "1"], out)
        self.assertFalse((self.root / "d/link").exists())
        self.assertTrue((self.root / "d/new").is_dir())

    def test_a_trapped_call_still_stops_the_fast_path_for_the_process(self):
        self.assertEqual(["REFUSED", "0"], self.ask("trap", "symlinkat", "symlink", "/d/link", "on"))

    def test_a_symlink_is_made_where_the_filesystem_allows_it(self):
        self.assertEqual(["ok", "1"], self.ask("symlink", "/d/link", "on"))
        self.assertEqual("target", os.readlink(self.root / "d/link"))

    def test_refused_calls_still_close_the_descriptors_they_opened(self):
        host = str(self.root / "d")
        out = self.ask("nosys", "readlinkat", *[op for _ in range(40) for op in ("canon", host)], "on", "fds")
        self.assertEqual(["REFUSED"] * 40 + ["1", "0"], out)
        self.assertEqual(["REFUSED", "0", "0"], self.ask("trap", "readlinkat", "canon", host, "on", "fds"))

    def test_wrappers_that_hand_a_refused_call_to_libc_do_not_refuse_the_next_one(self):
        # The filesystem answers the call with ENOSYS once; the wrapper hands it to libc, and the next call,
        # with the filesystem behaving, takes the fast path again.
        for nr, op, arg in [("openat", "opendir", "/d"), ("openat", "fopen", "/d/file"),
                            ("newfstatat", "realpath", "/d/file"), ("readlinkat", "getcwd", None),
                            ("openat", "nprocs", None), ("openat", "getpwuid", None)]:
            with self.subTest(op=op):
                call = [op] + ([arg] if arg else [])
                out = self.ask("nosys", nr, *call, "flag", "on", "nosys", "none", *call, "flag")
                self.assertEqual(["libc", "0", "1", "ok", "0"], out)

    def test_socket_lookups_that_fail_do_not_refuse_the_socket_call(self):
        self.assertEqual(["listening", "ok", "0", "ok", "0"],
                         self.ask("listen", "/s", "nosys", "fcntl", "accept", "flag", "nosys", "none", "accept", "flag"))
        self.assertEqual(["listening", "/t", "0", "1"],
                         self.ask("listen", "/t", "nosys-fd", "newfstatat", "getsockname", "flag", "on"))

    def test_a_connect_that_was_made_is_not_made_again(self):
        # A host path too long for sun_path is reached through the folder's descriptor; the socket's inode is
        # looked up afterwards to remember the name.
        guest = "/" + "x" * (110 - len(str(self.root))) + "/s"
        (self.root / guest[1:]).parent.mkdir()
        self.assertGreater(len(str(self.root)) + len(guest), 108)
        self.assertEqual(["listening", "ok", "0", "1"],
                         self.ask("listen", guest, "nosys-fd", "newfstatat", "connect", guest, "flag", "on"))


if __name__ == "__main__":
    unittest.main()
