import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import urllib.request

PROOT_DIR = Path(__file__).resolve().parents[1] / "proot"
BENCH = PROOT_DIR / "bench/fchdir.py"
FCHDIR_PATCH = "0028-fchdir-open-directory.patch"
CACHE = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "droiddeck-tests/proot"

ASHMEM_H = """#include <linux/ioctl.h>
#define __ASHMEMIOC 0x77
#define ASHMEM_SET_SIZE _IOW(__ASHMEMIOC, 3, size_t)
#define ASHMEM_GET_SIZE _IO(__ASHMEMIOC, 4)
"""

# The guest's fchdir round trips into a folder; proot's stat calls on it are counted with strace.
ROUTE = """import os, sys
fd = os.open(sys.argv[1], os.O_RDONLY | os.O_DIRECTORY)
top = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
for _ in range(int(sys.argv[2])):
    os.fchdir(fd)
    os.fchdir(top)
"""


def unavailable(reason):
    # CI installs the tools (build.yml, python-tests), so there a missing one fails the test.
    if os.environ.get("GITHUB_ACTIONS"):
        raise AssertionError(reason)
    raise unittest.SkipTest(reason)


def source_env():
    values = {}
    for line in (PROOT_DIR / "source.env").read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def fetch(name, urls, sha256):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == sha256:
        return path
    for url in urls:
        try:
            with urllib.request.urlopen(url, timeout=90) as response:
                data = response.read()
        except OSError:
            continue
        if hashlib.sha256(data).hexdigest() == sha256:
            path.write_bytes(data)
            return path
    raise unittest.SkipTest(f"cannot fetch {name}")


def extract(archive, into):
    into.mkdir(parents=True)
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            parts = member.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            member.name = parts[1]
            tar.extract(member, into, filter="tar")


def build(work, patches, proot_tar, talloc_tar):
    """Builds proot with the host compiler, as tools/proot/build.sh builds it with the NDK."""
    src = work / "proot"
    extract(proot_tar, src)
    shutil.copy(PROOT_DIR / "statx_mnt_id.h", src / "src/tracee/statx_mnt_id.h")
    shutil.copy(PROOT_DIR / "profile.h", src / "src/syscall/profile.h")
    for patch in patches:
        subprocess.run(["patch", "-d", str(src), "-p1", "--forward", "--quiet", "-i", str(patch)], check=True)
    talloc = work / "talloc"
    extract(talloc_tar, talloc)
    (talloc / "config").mkdir()
    shutil.copy(PROOT_DIR / "talloc-config.h", talloc / "config/config.h")
    subprocess.run(["gcc", "-c", "-O2", "-fPIC", "-include", "limits.h", "-include", "stdint.h", "-include", "string.h",
                    "-D__STDC_WANT_LIB_EXT1__=1", "-DHAVE_CONFIG_H", f"-I{talloc}/config", f"-I{talloc}",
                    f"-I{talloc}/lib/replace", "-o", str(work / "talloc.o"), str(talloc / "talloc.c")], check=True)
    subprocess.run(["ar", "rcs", str(work / "libtalloc.a"), str(work / "talloc.o")], check=True)
    (work / "shim/linux").mkdir(parents=True)
    (work / "shim/linux/ashmem.h").write_text(ASHMEM_H)
    make = ["make", "-C", str(src / "src"), f"-j{os.cpu_count() or 2}", "CC=gcc", "LD=gcc -fuse-ld=lld",
            "HAS_LOADER_32BIT=", "PROOT_UNBUNDLE_LOADER=/nonexistent", f"LDFLAGS={work}/libtalloc.a",
            f"CPPFLAGS=-D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -I. -I{talloc} -I{work}/shim -DARG_MAX=131072 "
            "-Wno-error=implicit-function-declaration"]
    subprocess.run(make + ["loader/loader"], check=True, capture_output=True)
    # loader-info.awk needs gawk's strtonum; the same offset from readelf.
    symbols = {}
    for line in subprocess.run(["readelf", "-sW", str(src / "src/loader/loader")], check=True, capture_output=True,
                               text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[7] in ("pokedata_workaround", "_start"):
            symbols[fields[7]] = int(fields[1], 16)
    if len(symbols) == 2:
        (src / "src/loader/loader-info.c").write_text(
            "#include <unistd.h>\nconst ssize_t offset_to_pokedata_workaround=%d;\n"
            % (symbols["pokedata_workaround"] - symbols["_start"]))
    subprocess.run(make + ["proot"], check=True, capture_output=True)
    return src / "src/proot", src / "src/loader/loader"


class ProotFchdirTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("gcc", "ld.lld", "make", "patch", "readelf", "python3", "strace"):
            if shutil.which(tool) is None:
                unavailable(f"{tool} is not available")
        env = source_env()
        proot_tar = fetch(f"proot-{env['PROOT_COMMIT']}.tar.gz",
                          [f"https://github.com/termux/proot/archive/{env['PROOT_COMMIT']}.tar.gz"], env["PROOT_SHA256"])
        talloc_tar = fetch(f"talloc-{env['TALLOC_VERSION']}.tar.gz",
                           [f"https://www.samba.org/ftp/talloc/talloc-{env['TALLOC_VERSION']}.tar.gz",
                            f"https://distfiles.macports.org/talloc/talloc-{env['TALLOC_VERSION']}.tar.gz"],
                           env["TALLOC_SHA256"])
        cls.work = Path(tempfile.mkdtemp())
        patches = sorted((PROOT_DIR / "patches").glob("*.patch"))
        cls.proot = build(cls.work / "with", patches, proot_tar, talloc_tar)
        cls.baseline = build(cls.work / "without", [p for p in patches if p.name != FCHDIR_PATCH], proot_tar, talloc_tar)
        probe = cls.run_guest(cls.proot, ["true"], check=False)
        if probe.returncode != 0:
            unavailable("proot cannot trace here: " + probe.stderr[-300:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    @classmethod
    def guest_tree(cls):
        """A rootfs folder with /storage bound beside it, a bound folder under /mnt and one under glue."""
        t = Path(tempfile.mkdtemp(dir=cls.work))
        root = t / "rootfs"
        for d in ["tmp", "mnt", "proc", "dev", "usr", "etc"]:
            (root / d).mkdir(parents=True)
        for name in ["bin", "sbin", "lib", "lib64"]:
            if (Path("/usr") / name).is_dir():
                (root / name).symlink_to("usr/" + name)
        shutil.copy(BENCH, root / "fchdir.py")
        (root / "route.py").write_text(ROUTE)
        (root / "opt/fchdir-root/a/b").mkdir(parents=True)
        (t / "storage/emulated/0/Android/data/fchdir-deep/a/b/c").mkdir(parents=True)
        (t / "fchdir-sd/games").mkdir(parents=True)
        (t / "fchdir-glued/x").mkdir(parents=True)
        binds = ["/usr", "/etc", "/proc", "/dev", f"{t}/storage:/storage", f"{t}/fchdir-sd:/mnt/droiddeck-sd",
                 f"{t}/fchdir-glued:/mnt/glue/a/sd"]
        return t, root, binds

    @classmethod
    def run_guest(cls, proot, argv, tree=None, trace=None, check=True):
        t, root, binds = tree or cls.guest_tree()
        binary, loader = proot
        cmd = [str(binary), "-r", str(root), "-w", "/"]
        for spec in binds:
            cmd += ["-b", spec]
        cmd += argv
        if trace:
            cmd = ["strace", "-o", str(trace), "-e", "trace=lstat,stat,newfstatat,statx"] + cmd
        tmp = t / "proot-tmp"
        tmp.mkdir(exist_ok=True)
        env = {"PATH": "/usr/bin:/bin", "PROOT_LOADER": str(loader), "PROOT_TMP_DIR": str(tmp), "LANG": "C.UTF-8"}
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300, env=env)
        if check and result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)
        return result

    @staticmethod
    def normal(text):
        lines = [line for line in text.splitlines() if not line.startswith("fchdir_us")]
        return [re.sub(r"/proc/\d+", "/proc/PID", re.sub(r"fchdir-[a-z0-9_]{8}", "fchdir-TMP", line)) for line in lines]

    def test_fchdir_lands_where_the_old_walk_did(self):
        extra = ["/opt/fchdir-root/a/b", "/storage/emulated/0/Android/data/fchdir-deep/a/b/c", "/storage",
                 "/mnt/droiddeck-sd/games", "/mnt/droiddeck-sd", "/mnt/glue/a/sd/x", "/mnt/glue/a/sd"]
        new = self.run_guest(self.proot, ["python3", "/fchdir.py", *extra])
        old = self.run_guest(self.baseline, ["python3", "/fchdir.py", *extra])
        self.assertEqual(self.normal(old.stdout), self.normal(new.stdout))
        for path in extra:
            self.assertIn(f"{path} ('{path}', '{path}', True)", new.stdout.splitlines())
        self.assertIn("relative (True, True, True, True)", new.stdout.splitlines())
        self.assertIn("renamed True", new.stdout.splitlines())

    def walked(self, proot, path, marker, rounds):
        tree = self.guest_tree()
        trace = tree[0] / f"trace-{rounds}"
        self.run_guest(proot, ["python3", "/route.py", path, str(rounds)], tree=tree, trace=trace)
        return sum(marker in line for line in trace.read_text().splitlines())

    def test_fchdir_takes_the_open_directory_instead_of_walking_it(self):
        # In the rootfs, in a binding whose parent is /, in one under a rootfs folder, and in one under glue.
        for path, marker in [("/opt/fchdir-root/a/b", "fchdir-root"),
                             ("/storage/emulated/0/Android/data/fchdir-deep/a/b/c", "fchdir-deep"),
                             ("/mnt/droiddeck-sd/games", "fchdir-sd"), ("/mnt/glue/a/sd/x", "fchdir-glued")]:
            with self.subTest(path=path):
                new = self.walked(self.proot, path, marker, 20) - self.walked(self.proot, path, marker, 0)
                old = self.walked(self.baseline, path, marker, 20) - self.walked(self.baseline, path, marker, 0)
                self.assertGreaterEqual(old, 20)
                self.assertEqual(0, new)


if __name__ == "__main__":
    unittest.main()
