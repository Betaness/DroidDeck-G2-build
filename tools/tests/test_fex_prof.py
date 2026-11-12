"""mali-g2/tools/fex-prof: FEX stats reader, guest snapshot, profile report and the guest env switch."""
import gzip
import importlib.util
import json
import mmap
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "mali-g2", "tools", "fex-prof")
HERE = os.path.normpath(HERE)
sys.path.insert(0, HERE)
import fexstats  # noqa: E402
import guestsnap  # noqa: E402

spec = importlib.util.spec_from_file_location("prof_report", os.path.join(HERE, "prof-report.py"))
prof_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prof_report)

PID = 4242
JIT = 0x7F0000000000
MISC = 0x7F1000000000
FEXDLL = 0x6FFF00000000
NTDLL_SO = 0x7E0000000000
D3D11 = 0x6FFE00000000
ANONPE = 0x6FFD00000000
MALI = 0x7D0000000000


def save(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f)


def load(path):
    with open(path) as f:
        return json.load(f)


def stats_blob(slots, size=4096):
    head = fexstats.HEADER.pack(2, 2, 112, b"FEX-2611-test", 64, size, 0)
    data = bytearray(size)
    data[:len(head)] = head
    for i, (tid, values) in enumerate(slots):
        off = 64 + i * 112
        struct.pack_into("<II", data, off, 0, tid)
        struct.pack_into("<12Q", data, off + 8, *values)
    return bytes(data)


class FexStats(unittest.TestCase):
    def test_parse_skips_free_slots(self):
        blob = stats_blob([(0x120, [1000] + [0] * 11), (0, [5] * 12), (0x2a4, [0, 0, 3] + [0] * 9)])
        header, slots = fexstats.parse(blob)
        self.assertEqual(header["fex_version"], "FEX-2611-test")
        self.assertEqual(header["app_type"], "win-arm64ec")
        self.assertEqual([(s["slot"], s["tid"]) for s in slots], [(0, 0x120), (2, 0x2A4)])
        self.assertEqual(slots[1]["sigbus"], 3)

    def test_rates(self):
        a = {"pid": 1, "time": 10.0, "header": {"fex_version": "v", "app_type": "win-arm64ec"},
             "threads": [{"slot": 0, "tid": 7, **{n: 0 for n, _ in fexstats.COUNTERS}}]}
        b = json.loads(json.dumps(a))
        b["time"] = 12.0
        b["threads"][0]["jit_time"] = 4_000_000  # ticks at 1 GHz over 2 s
        b["threads"][0]["cache_miss"] = 100
        b["threads"].append({"slot": 1, "tid": 9, **{n: 2 for n, _ in fexstats.COUNTERS}})
        r = fexstats.rates(a, b, 1e9)
        self.assertEqual(r["threads"][0]["jit_time_ms_s"], 2.0)
        self.assertEqual(r["threads"][0]["cache_miss_s"], 50.0)
        self.assertTrue(r["threads"][1]["new"])

    def test_short_slot_size_is_refused(self):
        blob = bytearray(stats_blob([]))
        struct.pack_into("<H", blob, 2, 40)
        with self.assertRaises(ValueError):
            fexstats.parse(bytes(blob))


def write_snapshot(d, perfmap_lines=None):
    os.makedirs(d, exist_ok=True)
    maps = [
        "%x-%x rwxp 00000000 00:00 0                          [anon:FEXMemJIT]" % (JIT, JIT + 0x1000000),
        "%x-%x r-xp 00000000 00:00 0                          [anon:FEXMem_Misc]" % (MISC, MISC + 0x10000),
        "%x-%x r--p 00000000 fd:00 11  /data/data/pkg/files/rootfs/proton/files/lib/wine/aarch64-windows/libarm64ecfex.dll" % (FEXDLL, FEXDLL + 0x1000),
        "%x-%x r-xp 00001000 fd:00 11  /data/data/pkg/files/rootfs/proton/files/lib/wine/aarch64-windows/libarm64ecfex.dll" % (FEXDLL + 0x1000, FEXDLL + 0x400000),
        "%x-%x r-xp 00000000 fd:00 12  /data/data/pkg/files/rootfs/proton/files/lib/wine/aarch64-unix/ntdll.so" % (NTDLL_SO, NTDLL_SO + 0x100000),
        "%x-%x r-xp 00001000 fd:00 13  /data/data/pkg/files/rootfs/pfx/drive_c/windows/system32/d3d11.dll" % (D3D11 + 0x1000, D3D11 + 0x200000),
        "%x-%x r--p 00000000 fd:00 13  /data/data/pkg/files/rootfs/pfx/drive_c/windows/system32/d3d11.dll" % (D3D11, D3D11 + 0x1000),
        "%x-%x r-xp 00000000 00:00 0 " % (ANONPE, ANONPE + 0x100000),
        "%x-%x r-xp 00000000 fd:01 14  /vendor/lib64/hw/vulkan.mali.so" % (MALI, MALI + 0x1000000),
    ]
    with open(os.path.join(d, "maps.txt"), "w") as f:
        f.write("\n".join(maps) + "\n")
    save(os.path.join(d, "ids.json"), {"pid": PID, "main_tid": PID, "comm": "Duckov.exe"})
    save(os.path.join(d, "tasks.json"), {"time": 100.0, "clk_tck": 100,
                                          "threads": [{"tid": PID, "comm": "Duckov.exe", "utime": 1000, "stime": 50, "cpu": 7, "state": "R"}],
                                          "procs": [{"pid": 77, "comm": "wineserver", "utime": 10, "stime": 30, "cpu": 1, "state": "S"}]})
    save(os.path.join(d, "env.json"), {"FEX_BLOCKJITNAMING": "1", "FEX_DISKCACHE": "0", "FEXPROF_MODE": "block", "WINEDEBUG": "-all"})
    save(os.path.join(d, "pe.json"), {"images": [{"base": ANONPE, "size": 0x100000, "machine": "0xaa64", "pe32plus": True, "name": "win32u.dll", "file": ""}]})
    if perfmap_lines is not None:
        with gzip.open(os.path.join(d, "perfmap.txt.gz"), "wt", newline="") as f:
            f.write("".join(l + "\r\n" for l in perfmap_lines))


def sample(ip, tid=PID, misc=2, t=0):
    return ("record sample: type 9, misc 0x%x, size 56\n  sample_type: 0x10107\n  ip 0x%x\n  pid %d, tid %d\n  time %d\n  id 1\n"
            "  cpu 7, res 0\n  period 250000\n" % (misc, ip, PID, tid, t))


class Report(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, perfmap):
        before, after = os.path.join(self.dir, "before"), os.path.join(self.dir, "after")
        write_snapshot(before)
        write_snapshot(after, perfmap)
        t = load(os.path.join(after, "tasks.json"))
        t["time"], t["threads"][0]["utime"], t["threads"][0]["stime"] = 110.0, 1900, 100
        t["procs"][0]["utime"], t["procs"][0]["stime"] = 20, 80
        save(os.path.join(after, "tasks.json"), t)
        body = ["record mmap2: type 10, misc 0x2, size 120\n  pid %d, tid %d, addr 0x%x, len 0x1000\n"
                "  pgoff 0x0, maj 0, min 0, ino 0, ino_generation 0\n  prot 5, flags 2, filename /data/app/x/lib/arm64/libextra.so\n"
                "  sample_id: pid %d, tid %d\n" % (PID, PID, 0x5000000000, PID, PID),
                "record comm: type 3, misc 0x0, size 40\n  pid %d, tid %d, comm Duckov.exe\n" % (PID, PID)]
        ips = ([JIT + 0x100] * 40 + [JIT + 0x2010] * 20 + [JIT + 0x5000] * 10 + [JIT + 0x9000] * 5 + [MISC + 0x40] * 5 +
               [FEXDLL + 0x2345] * 6 + [NTDLL_SO + 0x10] * 4 + [D3D11 + 0x3000] * 3 + [ANONPE + 0x50] * 2 + [MALI + 0x10] * 2 +
               [0x5000000000 + 8] * 1 + [0x1234] * 1)
        for i, ip in enumerate(ips):
            body.append(sample(ip, t=1_000_000_000 + i * 100_000_000))
        body.append(sample(0xFFFFFFC010000000, misc=1, t=1_000_000_000))
        body.append(sample(JIT + 0x100, tid=PID + 1, t=1_000_000_000))
        dump = os.path.join(self.dir, "x.dump.txt.gz")
        with gzip.open(dump, "wt") as f:
            f.write("".join(body))
        return dump, [before, after]

    def test_categories_with_block_symbols(self):
        perfmap = [
            "0x%x 200 z:\\games\\duckov\\unityplayer.dll+0x1a2b (0x%x)" % (JIT + 0x80, JIT + 0x80),
            "0x%x 400 c:\\program files\\duckov\\monobleedingedge\\embedruntime\\mono-2.0-bdwgc.dll+0x77 (0x%x)" % (JIT + 0x2000, JIT + 0x2000),
            "0x%x 100 JIT_0x1f2e3d4c_0x%x" % (JIT + 0x5000, JIT + 0x5000),
            "0x%x 300 Dispatch_17" % (MISC, ),
        ]
        dump, snaps = self.build(perfmap)
        res = prof_report.report(dump, snaps, tids=[PID])
        t = res["threads"][0]
        self.assertTrue(t["main"])
        self.assertEqual(t["samples"], 100)
        cats = {c["category"]: c for c in t["categories"]}
        mods = {m["module"]: m["samples"] for m in cats["fex-jit"]["modules"]}
        self.assertEqual(mods, {"jit:unityplayer.dll": 40, "jit:mono-2.0-bdwgc.dll": 20, "jit:outside-images": 10, "jit:no-symbol": 5})
        self.assertEqual(cats["fex-dispatcher"]["samples"], 5)
        self.assertEqual(cats["fex-runtime"]["samples"], 6)
        self.assertEqual(cats["wine-unix"]["samples"], 4)
        self.assertEqual(cats["dxvk"]["samples"], 3)
        self.assertEqual(cats["wine-pe"]["modules"][0]["module"], "win32u.dll")
        self.assertEqual(cats["gpu-driver"]["samples"], 2)
        self.assertEqual(cats["other-so"]["samples"], 1)
        self.assertEqual(cats["kernel"]["samples"], 1)
        self.assertEqual(cats["unknown"]["samples"], 1)
        top = {(x["module"], x["location"]): x["samples"] for x in t["top"]}
        self.assertEqual(top[("jit:unityplayer.dll", "+0x1a2b")], 40)
        self.assertEqual(top[("jit:outside-images", "0x1f2e3d4c")], 10)
        self.assertEqual(top[("libarm64ecfex.dll", "rva 0x2345")], 6)
        self.assertEqual(top[("d3d11.dll", "rva 0x3000")], 3)
        self.assertEqual(t["cpu"], {"dt": 10.0, "busy": 0.95, "user": 0.9, "system": 0.05})
        self.assertEqual(res["env"], {"FEXPROF_MODE": "block", "FEX_BLOCKJITNAMING": "1", "FEX_DISKCACHE": "0"})
        self.assertEqual(res["perfmap"]["lines"], 4)
        self.assertEqual(res["processes"], [{"pid": 77, "comm": "wineserver", "cpus": 0.06, "system": 0.05}])

    def test_run_without_symbols_uses_mapping_names(self):
        dump, snaps = self.build(None)
        res = prof_report.report(dump, snaps, tids=[PID])
        cats = {c["category"]: c for c in res["threads"][0]["categories"]}
        self.assertEqual(cats["fex-jit"]["samples"], 75)
        self.assertEqual([m["module"] for m in cats["fex-jit"]["modules"]], ["jit:no-symbol"])
        self.assertEqual(cats["fex-dispatcher"]["samples"], 5)

    def test_naming_run_without_symbol_file(self):
        dump, snaps = self.build(None)
        for d in snaps:
            with open(os.path.join(d, "maps.txt")) as f:
                text = f.read().replace("[anon:FEXMemJIT]", "")
            with open(os.path.join(d, "maps.txt"), "w") as f:
                f.write(text)
        res = prof_report.report(dump, snaps, tids=[PID])
        cats = {c["category"]: c for c in res["threads"][0]["categories"]}
        self.assertEqual(cats["fex-jit"]["samples"], 75)
        self.assertNotIn("anon-exec", cats)

    def test_snapshot_reads_a_stats_file(self):
        f = os.path.join(self.dir, "fex-1-stats")
        with open(f, "wb") as fh:
            fh.write(stats_blob([(0x120, list(range(12)))]))
        snap = fexstats.snapshot(1, f, 1e9)
        self.assertEqual(snap["threads"][0]["cache_miss"], 5)
        self.assertEqual(snap["header"]["fex_version"], "FEX-2611-test")

    def test_default_thread_choice_and_cli(self):
        dump, snaps = self.build([])
        out = subprocess.run([sys.executable, os.path.join(HERE, "prof-report.py"), dump] + snaps + ["--json", os.path.join(self.dir, "r.json")],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("[main]", out.stdout)
        res = load(os.path.join(self.dir, "r.json"))
        self.assertEqual([t["tid"] for t in res["threads"]], [PID])  # the other thread has 1 of 102 samples

    def test_classify_file(self):
        c = prof_report.classify_file
        self.assertEqual(c("/x/lib/wine/aarch64-windows/libwow64fex.dll"), "fex-runtime")
        self.assertEqual(c("/x/lib/wine/aarch64-windows/ntdll.dll"), "wine-pe")
        self.assertEqual(c("/x/lib/wine/dxvk/aarch64-windows/dxgi.dll"), "dxvk")
        self.assertEqual(c("/x/files/bin/wine-preloader"), "wine-loader")
        self.assertEqual(c("/x/usr/local/lib/libdroiddeck_hybris_vk.so"), "hybris")
        self.assertEqual(c("/system/lib64/libvulkan.so"), "gpu-driver")
        self.assertEqual(c("/apex/com.android.runtime/lib64/bionic/libc.so"), "android-libs")
        self.assertEqual(c("/x/usr/lib/aarch64-linux-gnu/libvulkan.so.1"), "vulkan-loader")
        self.assertEqual(c("/x/usr/lib/aarch64-linux-gnu/libc.so.6"), "glibc")
        self.assertEqual(c("/x/game/plugins/steam_api64.dll"), "other-pe")


class GuestSnap(unittest.TestCase):
    def test_pe_scan_finds_an_image_in_anonymous_memory(self):
        try:
            open("/proc/self/mem", "rb").close()
        except OSError:
            self.skipTest("/proc/self/mem not readable here")
        size = 0x3000
        buf = mmap.mmap(-1, size, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
        head = bytearray(0x1000)
        head[0:2] = b"MZ"
        struct.pack_into("<I", head, 0x3C, 0x80)
        head[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<H", head, 0x84, 0xAA64)
        opt = 0x80 + 24
        struct.pack_into("<H", head, opt, 0x20B)
        struct.pack_into("<I", head, opt + 56, size)
        struct.pack_into("<I", head, opt + 112, 0x1000)  # export directory RVA
        buf.write(bytes(head))
        export = bytearray(40)
        struct.pack_into("<I", export, 12, 0x1100)
        buf.seek(0x1000)
        buf.write(bytes(export))
        buf.seek(0x1100)
        buf.write(b"fakepe.dll\0")
        import ctypes
        base = ctypes.addressof(ctypes.c_char.from_buffer(buf))
        # a read-only header page starts its own mapping, as Wine's image views do
        libc = ctypes.CDLL(None, use_errno=True)
        self.assertEqual(libc.mprotect(ctypes.c_void_p(base), 0x1000, mmap.PROT_READ), 0)
        try:
            res = guestsnap.pe_scan(os.getpid(), guestsnap.maps_lines(os.getpid()))
            mine = [i for i in res["images"] if i["base"] == base]
            self.assertEqual(len(mine), 1, res.get("error"))
            self.assertEqual(mine[0]["name"], "fakepe.dll")
            self.assertEqual(mine[0]["machine"], "0xaa64")
            self.assertEqual(mine[0]["size"], size)
        finally:
            del base
            import gc
            gc.collect()

    def test_perfmap_candidates(self):
        with tempfile.TemporaryDirectory() as d:
            pfx = os.path.join(d, "pfx")
            os.makedirs(os.path.join(pfx, "dosdevices"))
            os.makedirs(os.path.join(pfx, "drive_c"))
            os.symlink("../drive_c", os.path.join(pfx, "dosdevices", "c:"))
            os.symlink("/", os.path.join(pfx, "dosdevices", "z:"))
            c = guestsnap.perfmap_candidates(77, {"STEAM_COMPAT_DATA_PATH": d})
            self.assertEqual(c[0], "/tmp/perf-77.map")
            self.assertIn(os.path.join(pfx, "drive_c", "tmp", "perf-77.map"), c)
            self.assertEqual(len(c), 2)


class GuestEnv(unittest.TestCase):
    def run_env(self, home, *args):
        return subprocess.run(["sh", os.path.join(HERE, "prof-guest.sh"), "env"] + list(args), env={"HOME": home, "PATH": os.environ["PATH"]},
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)

    def test_set_and_unset(self):
        with tempfile.TemporaryDirectory() as home:
            os.makedirs(os.path.join(home, ".config", "droiddeck"))
            f = os.path.join(home, ".config", "droiddeck", "game-environment.json")
            save(f, {"shared": {"A": "1"}, "games": {"3167020": {"FEX_DISABLEL2CACHE": "0"}}})
            r = self.run_env(home, "set")
            self.assertEqual(r.returncode, 0, r.stdout)
            g = load(f)["games"]["3167020"]
            self.assertEqual(g, {"FEX_DISABLEL2CACHE": "0", "FEX_BLOCKJITNAMING": "1", "FEX_DISKCACHE": "0", "FEXPROF_MODE": "block"})
            r = self.run_env(home, "set", "library")
            g = load(f)["games"]["3167020"]
            self.assertNotIn("FEX_BLOCKJITNAMING", g)
            self.assertEqual(g["FEX_LIBRARYJITNAMING"], "1")
            r = self.run_env(home, "unset")
            self.assertEqual(load(f)["games"]["3167020"], {"FEX_DISABLEL2CACHE": "0"})

    def test_refuses_foreign_entries(self):
        with tempfile.TemporaryDirectory() as home:
            os.makedirs(os.path.join(home, ".config", "droiddeck"))
            f = os.path.join(home, ".config", "droiddeck", "game-environment.json")
            save(f, {"games": {"3167020": {"FEX_DISKCACHE": "1"}}})
            r = self.run_env(home, "set")
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(load(f)["games"]["3167020"], {"FEX_DISKCACHE": "1"})
            r = self.run_env(home, "unset")
            self.assertEqual(load(f)["games"]["3167020"], {"FEX_DISKCACHE": "1"})


if __name__ == "__main__":
    unittest.main()
