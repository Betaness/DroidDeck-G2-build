import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

FASTPATH = Path(__file__).resolve().parents[1] / "proot/fastpath/fastpath.c"

# Drives the fast path with a stand-in trampoline. "trap NAME" answers that call with ENOSYS whatever its
# arguments, as proot does for a trampoline call a guest's seccomp filter traps; "nosys NAME" answers it with
# ENOSYS unless it carries the invalid arguments of the fast path's probe, as a filesystem without the call does.
PROGRAM = r"""
#include "FASTPATH"

static long trapped_nr = -1, unsupported_nr = -1;

static long host_sys(long nr, long a, long b, long c, long d, long e, long f) {
  if (nr == trapped_nr || (nr == unsupported_nr && a != -1)) return -ENOSYS;
  long r = syscall(nr, a, b, c, d, e, f);
  return r < 0 ? -errno : r;
}

static long number(const char *name) {
  if (!strcmp(name, "symlinkat")) return SYS_symlinkat;
  if (!strcmp(name, "readlinkat")) return SYS_readlinkat;
  if (!strcmp(name, "mkdirat")) return SYS_mkdirat;
  exit(5);
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
    else if (!strcmp(op, "nosys")) unsupported_nr = number(argv[++i]);
    else if (!strcmp(op, "symlink")) show(fp_symlink("target", AT_FDCWD, argv[++i]));
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

    def ask(self, *ops):
        result = subprocess.run([str(self.program), str(self.root), *ops], capture_output=True, text=True, timeout=60)
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


if __name__ == "__main__":
    unittest.main()
