"""mali-g2/tools/fex-prof: FEX stats reader, guest snapshot, profile report and the guest env switch."""
import ctypes
import gzip
import importlib.util
import json
import mmap
import os
import random
import struct
import subprocess
import sys
import tempfile
import time
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
    """A stats file laid out as FEX's SHMStats.h (STATS_VERSION 2) has it, field by field: header u8 Version @0,
    u8 app_type @1, u16 ThreadStatsSize @2, char fex_version[48] @4, u32 Head @52, u32 Size @56, u32 Pad @60; slots
    of 112 bytes from 64: u32 Next @0, u32 TID @4, 12 u64 counters @8..103, u64 Padding @104."""
    data = bytearray(size)
    data[0], data[1] = 2, 2
    struct.pack_into("<H", data, 2, 112)
    data[4:4 + 13] = b"FEX-2611-test"
    struct.pack_into("<I", data, 52, 64)
    struct.pack_into("<I", data, 56, size)
    for i, (tid, values) in enumerate(slots):
        off = 64 + i * 112
        struct.pack_into("<I", data, off, 0)
        struct.pack_into("<I", data, off + 4, tid)
        for k, v in enumerate(values):
            struct.pack_into("<Q", data, off + 8 + 8 * k, v)
        struct.pack_into("<Q", data, off + 104, 0xFFFFFFFFFFFFFFFF)
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

    def test_layout_matches_shmstats(self):
        self.assertEqual(fexstats.HEADER.size, 64)
        blob = stats_blob([(0x120, list(range(1, 13))), (0x2a4, [7] * 12)])
        header, slots = fexstats.parse(blob)
        self.assertEqual((header["version"], header["slot_size"], header["head"], header["size"]), (2, 112, 64, 4096))
        self.assertEqual([(s["slot"], s["tid"]) for s in slots], [(0, 0x120), (1, 0x2A4)])
        self.assertEqual([slots[0][n] for n, _ in fexstats.COUNTERS], list(range(1, 13)))
        self.assertEqual(slots[0]["jit_time"], 1)
        self.assertEqual(slots[0]["disk_cache_lookup_time"], 12)

    def test_short_slot_size_is_refused(self):
        blob = bytearray(stats_blob([]))
        struct.pack_into("<H", blob, 2, 40)
        with self.assertRaises(ValueError):
            fexstats.parse(bytes(blob))


def write_snapshot(d, perfmap_lines=None, eol="\n"):
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
            f.write("".join(l + eol for l in perfmap_lines))


def sample(ip, tid=PID, misc=2, t=0, chain=False):
    s = ("record sample: type 9, misc 0x%x, size 56\n  sample_type: 0x10107\n  ip 0x%x\n  pid %d, tid %d\n  time %d\n  id 1\n"
         "  cpu 7, res 0\n  period 250000\n" % (misc, ip, PID, tid, t))
    if chain:
        # simpleperf dump with --call-graph fp: the raw chain, then the symbolized frames two levels deep
        # (cmd_dumprecord.cpp ProcessSampleRecord), whose symbol names can read like fields
        s += ("  callchain nr=3\n    0x%x\n    0x7d0000001234\n    0x7d0000005678\n  callchain:\n"
              "    ip (/x/libfoo.so[+10])\n    time (/apex/com.android.runtime/lib64/bionic/libc.so[+1234])\n"
              "    period (/x/libbar.so[+20])\n" % ip)
    return s


def attr_section(event):
    return ("magic: PERFILE2\nattr_size: 136\nattr 1:\n  event_attr: for event type %s\n    type 1, size 136, config 0\n"
            "    sample_freq 4000\n  ids: 1\n" % event)


class Report(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, perfmap, eol="\n", chain=False, event=None):
        before, after = os.path.join(self.dir, "before"), os.path.join(self.dir, "after")
        write_snapshot(before)
        write_snapshot(after, perfmap, eol)
        t = load(os.path.join(after, "tasks.json"))
        t["time"], t["threads"][0]["utime"], t["threads"][0]["stime"] = 110.0, 1900, 100
        t["procs"][0]["utime"], t["procs"][0]["stime"] = 20, 80
        save(os.path.join(after, "tasks.json"), t)
        body = ["record mmap2: type 10, misc 0x2, size 120\n  pid %d, tid %d, addr 0x%x, len 0x1000\n"
                "  pgoff 0x0, maj 0, min 0, ino 0, ino_generation 0\n  prot 5, flags 2, filename /data/app/x/lib/arm64/libextra.so\n"
                "  sample_id: pid %d, tid %d\n" % (PID, PID, 0x5000000000, PID, PID),
                "record comm: type 3, misc 0x0, size 40\n  pid %d, tid %d, comm Duckov.exe\n" % (PID, PID)]
        if event:
            body.insert(0, attr_section(event))
        ips = ([JIT + 0x100] * 40 + [JIT + 0x2010] * 20 + [JIT + 0x5000] * 10 + [JIT + 0x9000] * 5 + [MISC + 0x40] * 5 +
               [FEXDLL + 0x2345] * 6 + [NTDLL_SO + 0x10] * 4 + [D3D11 + 0x3000] * 3 + [ANONPE + 0x50] * 2 + [MALI + 0x10] * 2 +
               [0x5000000000 + 8] * 1 + [0x1234] * 1)
        for i, ip in enumerate(ips):
            body.append(sample(ip, t=1_000_000_000 + i * 100_000_000, chain=chain))
        body.append(sample(0xFFFFFFC010000000, misc=1, t=1_000_000_000))
        body.append(sample(JIT + 0x100, tid=PID + 1, t=1_000_000_000))
        dump = os.path.join(self.dir, "x.dump.txt.gz")
        with gzip.open(dump, "wt") as f:
            f.write("".join(body))
        return dump, [before, after]

    PERFMAP = [
        "0x%x 200 z:\\games\\duckov\\unityplayer.dll+0x1a2b (0x%x)" % (JIT + 0x80, JIT + 0x80),
        "0x%x 400 c:\\program files\\duckov\\monobleedingedge\\embedruntime\\mono-2.0-bdwgc.dll+0x77 (0x%x)" % (JIT + 0x2000, JIT + 0x2000),
        "0x%x 100 JIT_0x1f2e3d4c_0x%x" % (JIT + 0x5000, JIT + 0x5000),
        "0x%x 300 Dispatch_17" % (MISC, ),
    ]

    def test_categories_with_block_symbols(self):
        self.check_categories(self.build(self.PERFMAP))

    def test_symbol_file_with_crlf(self):
        self.check_categories(self.build(self.PERFMAP, eol="\r\n"))

    def test_samples_with_call_chains(self):
        # the frames 'ip (...)', 'time (...)', 'period (...)' at the deeper indent are not the sample's fields
        self.check_categories(self.build(self.PERFMAP, chain=True))

    def check_categories(self, built):
        dump, snaps = built
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

    def test_tid_without_samples_is_an_error(self):
        dump, snaps = self.build(self.PERFMAP)
        with self.assertRaises(ValueError) as e:
            prof_report.report(dump, snaps, tids=[99])
        self.assertIn("%d (100)" % PID, str(e.exception))
        self.assertIn("%d (1)" % (PID + 1), str(e.exception))
        out = subprocess.run([sys.executable, os.path.join(HERE, "prof-report.py"), dump] + snaps + ["--tid", "99"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(out.returncode, 1)
        self.assertTrue(out.stderr.startswith("prof-report: no samples of thread 99"), out.stderr)
        self.assertNotIn("Traceback", out.stderr)
        res = prof_report.report(dump, snaps, tids=[99, PID])
        self.assertEqual([t["tid"] for t in res["threads"]], [PID])
        self.assertEqual(res["tids_without_samples"], [99])

    def test_no_thread_over_min_pct_and_no_main_thread_samples(self):
        dump, snaps = self.build(self.PERFMAP)
        for d in snaps:
            ids = load(os.path.join(d, "ids.json"))
            ids["main_tid"] = PID + 7
            save(os.path.join(d, "ids.json"), ids)
        with self.assertRaises(ValueError) as e:
            prof_report.report(dump, snaps, min_pct=100)
        self.assertIn("main thread (%d) has none" % (PID + 7), str(e.exception))

    def test_event_from_the_attr_section(self):
        res = prof_report.report(*self.build(self.PERFMAP, event="cpu-clock"), tids=[PID])
        t = res["threads"][0]
        self.assertEqual(res["events"], ["cpu-clock"])
        # 100 samples of 250000 ns over 9.8 s
        self.assertEqual(res["span_s"], 9.8)
        self.assertEqual(t["period_ms_per_s"], round(100 * 250000 / 1e6 / 9.8, 1))
        res = prof_report.report(*self.build(self.PERFMAP, event="cpu-cycles"), tids=[PID])
        t = res["threads"][0]
        self.assertEqual(res["events"], ["cpu-cycles"])
        self.assertIsNone(t["period_ms_per_s"])
        self.assertEqual(t["period_per_s"], round(100 * 250000 / 9.8))
        res = prof_report.report(*self.build(self.PERFMAP), tids=[PID])
        self.assertIsNone(res["threads"][0]["period_ms_per_s"])
        out = subprocess.run([sys.executable, os.path.join(HERE, "prof-report.py")] + list(self.build(self.PERFMAP, event="cpu-cycles")[:1])
                             + self.build(self.PERFMAP, event="cpu-cycles")[1], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("M cpu-cycles/s by sample period", out.stdout)
        self.assertNotIn("ms/s", out.stdout)

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


def snapshot_dir(d, maps, pe=(), perfmap=None, env=None):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "maps.txt"), "w") as f:
        for lo, hi, perms, off, name in maps:
            f.write("%x-%x %s %08x 00:00 0  %s\n" % (lo, hi, perms, off, name))
    save(os.path.join(d, "pe.json"), {"images": list(pe)})
    save(os.path.join(d, "env.json"), env if env is not None else {"FEX_BLOCKJITNAMING": "1"})
    if perfmap is not None:
        with gzip.open(os.path.join(d, "perfmap.txt.gz"), "wt") as f:
            f.write("".join(l + "\n" for l in perfmap))
    return prof_report.Snapshot(d)


def block(start, size, rva, path="z:\\games\\duckov\\unityplayer.dll"):
    return "0x%x %x %s+0x%x (0x%x)" % (start, size, path, rva, start)


class PerfMapLookup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def perfmap(self, lines):
        f = os.path.join(self.tmp.name, "perf.map")
        with open(f, "w") as fh:
            fh.write("".join(l + "\n" for l in lines))
        return prof_report.PerfMap(f)

    def test_multiblock_fragment_ends_at_the_fragment(self):
        # one fragment at 0x100000 of 0x200 bytes with a second subblock at +0x180: FEX writes both lines with the
        # fragment's size, so the second line alone would reach 0x100380
        pm = self.perfmap([block(0x100000, 0x200, 0x1a2b), block(0x100180, 0x200, 0x1a2b)])
        self.assertEqual(pm.find(0x100190), (0x100000, "z:\\games\\duckov\\unityplayer.dll+0x1a2b (0x100000)"))
        self.assertIsNone(pm.find(0x100200))
        self.assertIsNone(pm.find(0x100300))
        self.assertEqual(pm.lines, 2)

    def test_next_fragment_keeps_its_own_symbol(self):
        # the next fragment starts where the first one ends, and its lines can come before the first one's second
        # subblock line in the file (another thread's buffer)
        a, b = block(0x100000, 0x200, 0x10), block(0x100200, 0x80, 0x99, "c:\\x\\mono-2.0-bdwgc.dll")
        pm = self.perfmap([a, b, block(0x100180, 0x200, 0x10)])
        self.assertTrue(pm.find(0x100250)[1].startswith("c:\\x\\mono-2.0-bdwgc.dll+0x99"))
        self.assertTrue(pm.find(0x1001f0)[1].startswith("z:\\games\\duckov\\unityplayer.dll+0x10"))
        self.assertIsNone(pm.find(0x100280))

    def test_later_lines_win_where_ranges_overlap(self):
        # an old fragment, then (after a code cache clear) a new one over part of its range
        pm = self.perfmap([block(0x100000, 0x400, 0x10), "0x100100 100 JIT_0xabc_0x100100"])
        self.assertEqual(pm.find(0x100150), (0x100100, "JIT_0xabc_0x100100"))
        self.assertTrue(pm.find(0x100050)[1].endswith("+0x10 (0x100000)"))
        self.assertTrue(pm.find(0x100250)[1].endswith("+0x10 (0x100000)"))
        pm = self.perfmap(["0x100100 100 JIT_0xabc_0x100100", block(0x100000, 0x400, 0x10)])
        self.assertTrue(pm.find(0x100150)[1].endswith("+0x10 (0x100000)"))

    def test_newest_on_top_against_brute_force(self):
        rnd = random.Random(7)
        for _ in range(200):
            rows = []
            for i in range(rnd.randint(0, 12)):
                lo = rnd.randint(0, 60)
                rows.append((lo, lo + rnd.randint(0, 20), i))
            table = prof_report.newest_on_top(rows)
            for (lo1, hi1, _), (lo2, _, _) in zip(table, table[1:]):
                self.assertLessEqual(hi1, lo2)
            for addr in range(0, 85):
                want = [p for lo, hi, p in rows if lo <= addr < hi]
                got = [p for lo, hi, p in table if lo <= addr < hi]
                self.assertEqual(got, want[-1:], (rows, addr))


class Lookup(unittest.TestCase):
    FILE = 0x5500000000
    DV = 0x6000000000
    BUF = DV + 0x1000

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def resolver(self, perfmap):
        maps = [
            (self.FILE, self.FILE + 0x100000, "r-xp", 0, "/x/usr/lib/libfoo.so"),
            # a managed assembly mapped as data: SizeOfImage (0x10000) is more than the view
            (self.DV, self.DV + 0x1000, "r--p", 0, "/x/game/Duckov_Data/Managed/Assembly-CSharp.dll"),
            (self.BUF, self.BUF + 0x1000000, "rwxp", 0, ""),
            (ANONPE, ANONPE + 0x1000, "r--p", 0, ""),
            (ANONPE + 0x1000, ANONPE + 0x100000, "r-xp", 0, ""),
            (ANONPE + 0x200000, ANONPE + 0x201000, "r--p", 0, ""),
            (ANONPE + 0x201000, ANONPE + 0x240000, "r-xp", 0, ""),
        ]
        pe = [{"base": self.DV, "size": 0x10000, "machine": "0x14c", "pe32plus": False, "name": "Assembly-CSharp.dll",
               "file": "/x/game/Duckov_Data/Managed/Assembly-CSharp.dll"},
              {"base": ANONPE, "size": 0x100000, "machine": "0xaa64", "pe32plus": True, "name": "win32u.dll", "file": ""},
              {"base": ANONPE + 0x200000, "size": 0x40000, "machine": "0xaa64", "pe32plus": True, "name": "", "file": ""}]
        snap = snapshot_dir(os.path.join(self.tmp.name, "s"), maps, pe, perfmap)
        return prof_report.Resolver([snap], {}, PID)

    def test_old_symbols_do_not_claim_files_or_pe_images(self):
        r = self.resolver(["0x%x 100000 JIT_0xdead_0x%x" % (self.FILE, self.FILE),
                           "0x%x 100000 JIT_0xbeef_0x%x" % (ANONPE, ANONPE),
                           block(self.BUF + 0x100, 0x80, 0x1a2b)])
        self.assertEqual(r.resolve(self.FILE + 0x10, False), ("other-so", "libfoo.so", "off 0x10"))
        self.assertEqual(r.resolve(ANONPE + 0x1050, False), ("wine-pe", "win32u.dll", "rva 0x1050"))
        self.assertEqual(r.resolve(self.BUF + 0x110, False), ("fex-jit", "jit:unityplayer.dll", "+0x1a2b"))

    def test_data_view_does_not_claim_the_next_mapping(self):
        r = self.resolver([])
        self.assertEqual(r.resolve(self.BUF + 0x2000, False), ("fex-jit", "jit:no-symbol", None))

    def test_pe_image_without_export_name(self):
        r = self.resolver([])
        self.assertEqual(r.resolve(ANONPE + 0x201010, False), ("other-pe", "pe@0x%x" % (ANONPE + 0x200000), "rva 0x1010"))


def fake_pe(size, export_rva=0, name_rva=0):
    head = bytearray(0x1000)
    head[0:2] = b"MZ"
    struct.pack_into("<I", head, 0x3C, 0x80)
    head[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", head, 0x84, 0xAA64)
    opt = 0x80 + 24
    struct.pack_into("<H", head, opt, 0x20B)
    struct.pack_into("<I", head, opt + 56, size)
    struct.pack_into("<I", head, opt + 112, export_rva)
    return bytes(head)


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

    def anon_pages(self, n):
        try:
            open("/proc/self/mem", "rb").close()
        except OSError:
            self.skipTest("/proc/self/mem not readable here")
        buf = mmap.mmap(-1, n * 0x1000, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
        self.addCleanup(buf.close)
        return buf, ctypes.addressof(ctypes.c_char.from_buffer(buf))

    def test_pe_scan_looks_past_a_data_view(self):
        # rows as /proc/PID/maps would show a file's data view (SizeOfImage 0x10000, one page mapped) with an image
        # Wine read into anonymous memory right after it
        buf, base = self.anon_pages(3)
        buf.write(fake_pe(0x10000))
        buf.seek(0x1000)
        buf.write(fake_pe(0x2000))
        rows = [(base, base + 0x1000, "r--p", 0, "/x/game/Managed/Assembly-CSharp.dll"),
                (base + 0x1000, base + 0x3000, "r-xp", 0, "")]
        res = guestsnap.pe_scan(os.getpid(), rows)
        self.assertEqual([(i["base"] - base, i["size"], i["file"]) for i in res["images"]],
                         [(0, 0x10000, "/x/game/Managed/Assembly-CSharp.dll"), (0x1000, 0x2000, "")])

    def test_pe_scan_reads_export_names_only_from_scannable_mappings(self):
        buf, base = self.anon_pages(3)
        buf.write(fake_pe(0x3000, export_rva=0x2000))
        export = bytearray(40)
        struct.pack_into("<I", export, 12, 0x2100)
        buf.seek(0x2000)
        buf.write(bytes(export))
        buf.seek(0x2100)
        buf.write(b"fakepe.dll\0")
        rows = [(base, base + 0x2000, "r--p", 0, ""), (base + 0x2000, base + 0x3000, "rw-s", 0, "/dev/mali0")]
        self.assertEqual([i["name"] for i in guestsnap.pe_scan(os.getpid(), rows)["images"]], [""])
        rows[1] = (base + 0x2000, base + 0x3000, "rw-p", 0, "")
        self.assertEqual([i["name"] for i in guestsnap.pe_scan(os.getpid(), rows)["images"]], ["fakepe.dll"])

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
        return self.run_guest(home, "env", *args)

    def run_guest(self, home, *args, **env):
        e = {"HOME": home, "PATH": os.environ["PATH"], "OUTBOX": os.path.join(home, "outbox"), "GUEST_TMP": os.path.join(home, "gtmp")}
        e.update(env)
        return subprocess.run(["sh", os.path.join(HERE, "prof-guest.sh")] + list(args), env=e,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)

    def test_restore_removes_fex_symbol_files(self):
        with tempfile.TemporaryDirectory() as home:
            os.makedirs(os.path.join(home, ".config", "droiddeck"))
            f = os.path.join(home, ".config", "droiddeck", "game-environment.json")
            save(f, {"games": {}})
            pfx = os.path.join(home, ".local", "share", "Steam", "steamapps", "compatdata", "3167020", "pfx")
            zroot, gtmp = os.path.join(home, "zroot"), os.path.join(home, "gtmp")
            for d in (os.path.join(pfx, "drive_c"), os.path.join(pfx, "dosdevices"), os.path.join(zroot, "tmp"), gtmp):
                os.makedirs(d)
            os.symlink("../drive_c", os.path.join(pfx, "dosdevices", "c:"))
            os.symlink(zroot, os.path.join(pfx, "dosdevices", "z:"))
            old = os.path.join(gtmp, "perf-11.map")
            open(old, "w").close()
            past = time.time() - 100
            os.utime(old, (past, past))
            r = self.run_env(home, "set")
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn("created", r.stdout)
            marker = os.path.join(home, ".cache", "droiddeck-fexprof", "env-set")
            os.utime(marker, (past + 50, past + 50))
            new = [os.path.join(gtmp, "perf-12.map"), os.path.join(pfx, "drive_c", "tmp", "perf-13.map"),
                   os.path.join(zroot, "tmp", "perf-14.map")]
            for p in new:
                with open(p, "w") as fh:
                    fh.write("0x1 10 x\n")
            other = os.path.join(zroot, "tmp", "keep.txt")
            open(other, "w").close()
            r = self.run_guest(home, "restore")
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn("ok removed 3 perf-*.map", r.stdout)
            self.assertEqual([p for p in new if os.path.exists(p)], [])
            self.assertTrue(os.path.exists(old))
            self.assertTrue(os.path.exists(other))
            self.assertFalse(os.path.exists(os.path.join(pfx, "drive_c", "tmp")))
            self.assertFalse(os.path.exists(os.path.join(home, ".cache", "droiddeck-fexprof")))
            self.assertEqual(load(f), {"games": {}})

    def test_restore_keeps_a_tmp_it_did_not_create(self):
        with tempfile.TemporaryDirectory() as home:
            os.makedirs(os.path.join(home, ".config", "droiddeck"))
            save(os.path.join(home, ".config", "droiddeck", "game-environment.json"), {})
            ctmp = os.path.join(home, ".local", "share", "Steam", "steamapps", "compatdata", "3167020", "pfx", "drive_c", "tmp")
            os.makedirs(ctmp)
            os.makedirs(os.path.join(home, "gtmp"))
            self.assertEqual(self.run_env(home, "set").returncode, 0)
            self.assertEqual(self.run_guest(home, "restore").returncode, 0)
            self.assertTrue(os.path.isdir(ctmp))

    def test_usage_prints_the_header_only(self):
        for script, first in (("prof-guest.sh", "# prof-guest.sh"), ("prof-device.sh", "# prof-device.sh")):
            r = subprocess.run(["sh", os.path.join(HERE, script)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
            self.assertEqual(r.returncode, 2)
            lines = r.stdout.splitlines()
            self.assertEqual(lines[0].split(" (")[0], first)
            self.assertTrue(all(l.startswith("#") for l in lines), r.stdout)
            self.assertTrue(lines[-1].startswith("# env:"), lines[-1])

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

    def test_unset_drops_the_entry_it_created(self):
        with tempfile.TemporaryDirectory() as home:
            os.makedirs(os.path.join(home, ".config", "droiddeck"))
            f = os.path.join(home, ".config", "droiddeck", "game-environment.json")
            save(f, {"shared": {"A": "1"}})
            self.assertEqual(self.run_env(home, "set").returncode, 0)
            self.assertEqual(self.run_env(home, "unset").returncode, 0)
            self.assertEqual(load(f), {"shared": {"A": "1"}, "games": {}})

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
