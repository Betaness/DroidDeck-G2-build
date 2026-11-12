#!/usr/bin/env python3
"""prof-report.py: where a game's threads spend their CPU time, from Android simpleperf samples and what the guest
knows about the process (FEX's JIT symbol file, the names of anonymous mappings, PE images).

usage: prof-report.py DUMP SNAPDIR [SNAPDIR...] [--tid TID,...] [--min-pct P] [--top N] [--json OUT]

DUMP     `simpleperf dump` output of the recording (.gz is fine): prof-device.sh record pulls it.
SNAPDIR  guestsnap.py snapshots of the game process (prof-guest.sh snap), before and after the recording. Mappings
         are looked up in the last snapshot first, then the earlier ones, then the recording's own mmap records;
         FEX's symbol file is taken from the last snapshot that has one; FEX stats and thread CPU times are compared
         between the first and the last snapshot.

A sample is put in one category by its address:
  fex-jit         code FEX generated for guest x86 code. With FEX_BLOCKJITNAMING the block's own symbol says which
                  PE image the guest code is in (jit:unityplayer.dll ...) or, for code outside any image (Mono's
                  JIT output, stubs), jit:outside-images and the guest address. A block without a symbol is
                  jit:no-symbol (FEX keeps up to ~4 KB of the newest symbols of each thread unwritten, and blocks
                  loaded from its disk cache get none); in a run without symbols all of it is jit:no-symbol.
  fex-dispatcher  FEX's dispatcher code ([anon:FEXMem_Misc], or the Dispatch_ symbol)
  fex-runtime     libarm64ecfex.dll / libwow64fex.dll (compiler, lookup slow path, exception handling)
  fex-other       other FEX-named mappings
  wine-unix, wine-pe, wine-loader, dxvk, vkd3d, vulkan-loader, hybris, gpu-driver (Mali, Android's Vulkan
  loader), android-libs, glibc, other-so, other-pe: by the mapped file
  vdso, kernel (kernel-mode samples), anon-exec (other anonymous code), unknown (no mapping)
"""
import argparse
import bisect
import gzip
import json
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fexstats  # noqa: E402

ORDER = ("fex-jit", "fex-dispatcher", "fex-runtime", "fex-other", "wine-unix", "wine-pe", "wine-loader", "dxvk", "vkd3d",
         "vulkan-loader", "hybris", "gpu-driver", "android-libs", "glibc", "other-so", "other-pe", "vdso", "kernel",
         "anon-exec", "unknown")
DXVK = {"d3d11.dll", "dxgi.dll", "d3d10core.dll", "d3d10.dll", "d3d10_1.dll", "d3d9.dll", "d3d8.dll"}
VKD3D = {"d3d12.dll", "d3d12core.dll"}
GLIBC = re.compile(r"^(libc|libm|libpthread|libdl|librt|libresolv)\.so|^ld-linux")
PE_EXT = (".dll", ".exe", ".drv", ".sys", ".ocx", ".cpl")
ANDROID_ROOTS = ("/system/", "/apex/", "/vendor/", "/odm/", "/product/", "/system_ext/")
REC = re.compile(r"^record (\S+): type (\d+), misc 0x([0-9a-fA-F]+)")
PIDTID = re.compile(r"^pid (\d+), tid (\d+)$")
MMAP = re.compile(r"^pid (\d+), tid (\d+), addr 0x([0-9a-fA-F]+), len 0x([0-9a-fA-F]+)")
PGOFF = re.compile(r"^pgoff 0x([0-9a-fA-F]+)(?:, filename (.*))?")
FNAME = re.compile(r"^prot \d+, flags \d+, filename (.*)$")
COMM = re.compile(r"^pid (\d+), tid (\d+), comm (.*)$")
BLOCK = re.compile(r"^(.*)\+0x([0-9a-fA-F]+) \(0x[0-9a-fA-F]+\)$")
KERNEL_MIN = 0xFFFF000000000000


def opener(path):
    return gzip.open(path, "rt", errors="replace") if path.endswith(".gz") else open(path, errors="replace")


def winbase(path):
    return re.split(r"[\\/]", path)[-1].lower()


def classify_file(path):
    base = os.path.basename(path).lower()
    p = path.lower()
    if re.fullmatch(r"lib(arm64ec|wow64)fex\.dll", base):
        return "fex-runtime"
    if base in DXVK:
        return "dxvk"
    if base in VKD3D or base.startswith("libvkd3d"):
        return "vkd3d"
    if "/wine/" in p and "-unix/" in p:
        return "wine-unix"
    if base in ("wine", "wine64", "wine-preloader", "wine64-preloader", "wineserver"):
        return "wine-loader"
    if base.endswith(PE_EXT):
        return "wine-pe" if ("/wine/" in p or "/windows/" in p) else "other-pe"
    if "hybris" in p:
        return "hybris"
    if p.startswith(ANDROID_ROOTS):
        if "mali" in base or base.startswith(("vulkan.", "libvulkan.so")):
            return "gpu-driver"
        return "android-libs"
    if base.startswith("libvulkan.so"):
        return "vulkan-loader"
    if GLIBC.match(base):
        return "glibc"
    return "other-so"


class Intervals:
    """[lo, hi) ranges with payloads. One source (a maps copy, a PE scan) does not overlap itself; recorded mmaps can,
    and then the latest one covering an address wins."""

    def __init__(self, rows=()):
        self.rows = sorted(rows, key=lambda r: r[0])
        self.los = [r[0] for r in self.rows]

    def find(self, addr, depth=16):
        i = bisect.bisect_right(self.los, addr) - 1
        stop = max(i - depth, -1)
        while i > stop:
            lo, hi, payload = self.rows[i]
            if addr < hi:
                return lo, hi, payload
            i -= 1
        return None


def parse_maps(path):
    """[(lo, hi, perms, offset, name)] of a /proc/PID/maps copy."""
    rows = []
    with open(path, errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines:
        parts = line.split(None, 5)
        if len(parts) < 5 or "-" not in parts[0]:
            continue
        lo, hi = (int(x, 16) for x in parts[0].split("-"))
        rows.append((lo, hi, parts[1], int(parts[2], 16), parts[5] if len(parts) > 5 else ""))
    return rows


class PerfMap:
    """FEX's JIT symbol file: '<host address> <size hex> <name>' lines, written by the C runtime of FEX's Windows build
    (so possibly CRLF). The newest line for a start address wins; a lookup takes the closest start at or below the
    address whose range covers it (block symbols of a multiblock all carry the whole fragment's size)."""

    def __init__(self, path=None):
        self.starts, self.entries, self.spaces = [], [], Intervals()
        self.lines = self.bad = 0
        if path:
            self.load(path)

    def load(self, path):
        by_start, spaces = {}, []
        with opener(path) as f:
            lines = f.read().splitlines()
        for line in lines:
            if not line:
                continue
            self.lines += 1
            parts = line.split(" ", 2)
            try:
                start, size, name = int(parts[0], 16), int(parts[1], 16), parts[2]
            except (IndexError, ValueError):
                self.bad += 1
                continue
            if name == "FEXJIT":
                spaces.append((start, start + size, None))
            else:
                by_start[start] = (start + size, name)
        self.starts = sorted(by_start)
        self.entries = [by_start[s] for s in self.starts]
        self.spaces = Intervals(spaces)

    def find(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and addr < self.entries[i][0]:
            return self.starts[i], self.entries[i][1]
        return None

    def in_space(self, addr):
        return self.spaces.find(addr) is not None


def parse_dump(path):
    """samples [(pid, tid, ip, kernel, period, time)], mmaps {pid: [(lo, hi, pgoff, name)]}, comms {tid: comm}"""
    samples, mmaps, comms = [], defaultdict(list), {}
    cur = None

    def flush(c):
        if not c:
            return
        if c["type"] == "sample" and "ip" in c and "tid" in c:
            samples.append((c["pid"], c["tid"], c["ip"], (c["misc"] & 7) == 1 or c["ip"] >= KERNEL_MIN, c.get("period", 1), c.get("time")))
        elif c["type"] in ("mmap", "mmap2") and "addr" in c and "name" in c:
            mmaps[c["pid"]].append((c["addr"], c["addr"] + c["len"], c.get("pgoff", 0), c["name"]))
        elif c["type"] == "comm" and "comm" in c:
            comms[c["tid"]] = c["comm"]

    with opener(path) as stream:
        for line in stream:
            if not line.startswith(" "):
                flush(cur)
                m = REC.match(line)
                cur = {"type": m.group(1), "misc": int(m.group(3), 16)} if m else None
                continue
            if cur is None:
                continue
            s = line.strip()
            t = cur["type"]
            if s.startswith("sample_id:"):
                continue
            if t == "sample":
                if s.startswith("ip "):
                    cur["ip"] = int(s[3:], 16)
                elif s.startswith("period "):
                    cur["period"] = int(s[7:])
                elif s.startswith("time "):
                    cur["time"] = int(s[5:])
                else:
                    m = PIDTID.match(s)
                    if m:
                        cur["pid"], cur["tid"] = int(m.group(1)), int(m.group(2))
            elif t in ("mmap", "mmap2"):
                m = MMAP.match(s)
                if m:
                    cur["pid"], cur["addr"], cur["len"] = int(m.group(1)), int(m.group(3), 16), int(m.group(4), 16)
                    continue
                m = PGOFF.match(s)
                if m:
                    cur["pgoff"] = int(m.group(1), 16)
                    if m.group(2) is not None:
                        cur["name"] = m.group(2)
                    continue
                m = FNAME.match(s)
                if m:
                    cur["name"] = m.group(1)
            elif t == "comm":
                m = COMM.match(s)
                if m:
                    cur["tid"], cur["comm"] = int(m.group(2)), m.group(3)
    flush(cur)
    return samples, mmaps, comms


class Snapshot:
    def __init__(self, path):
        self.path = path
        def j(name):
            f = os.path.join(path, name)
            if not os.path.isfile(f):
                return None
            with open(f) as fh:
                return json.load(fh)
        self.ids, self.tasks, self.env, self.fex = j("ids.json"), j("tasks.json"), j("env.json"), j("fexstats.json")
        pe = j("pe.json") or {}
        self.pe = pe.get("images", [])
        self.perfmap_meta = j("perfmap.json")
        m = os.path.join(path, "maps.txt")
        self.maps = parse_maps(m) if os.path.isfile(m) else []
        pm = os.path.join(path, "perfmap.txt.gz")
        self.perfmap = pm if os.path.isfile(pm) else None


class Resolver:
    """Address lookups for one process. snaps are that process's snapshots (none for other processes of the
    recording, such as wineserver or the tracer): their maps, newest first, then the recording's mmap records."""

    def __init__(self, snaps, mmaps, pid):
        self.sources = [Intervals((lo, hi, (perms, off, name)) for lo, hi, perms, off, name in snap.maps) for snap in reversed(snaps)]
        recorded = mmaps.get(pid, [])
        self.sources.append(Intervals((lo, hi, ("r-xp", off, name)) for lo, hi, off, name in recorded))
        self.image_base = {}
        for snap in snaps:
            for lo, hi, perms, off, name in snap.maps:
                if off == 0 and name.startswith("/"):
                    self.image_base[name] = min(lo, self.image_base.get(name, lo))
        for lo, hi, off, name in recorded:
            if off == 0 and name.startswith("/"):
                self.image_base.setdefault(name, lo)
        self.pe = [Intervals((img["base"], img["base"] + img["size"], img) for img in snap.pe) for snap in reversed(snaps)]
        pm = next((s.perfmap for s in reversed(snaps) if s.perfmap), None)
        self.perfmap = PerfMap(pm)
        env = next((s.env for s in reversed(snaps) if s.env), None) or {}
        self.named_run = bool(pm) or any(env.get(k) == "1" for k in ("FEX_BLOCKJITNAMING", "FEX_LIBRARYJITNAMING", "FEX_GLOBALJITNAMING"))

    def mapping(self, ip):
        for src in self.sources:
            hit = src.find(ip)
            if hit:
                return hit
        return None

    def pe_image(self, ip):
        for src in self.pe:
            hit = src.find(ip, depth=1)
            if hit:
                return hit
        return None

    def resolve(self, ip, kernel):
        """(category, module, location) for one sample address."""
        if kernel:
            return "kernel", "kernel", None
        hit = self.perfmap.find(ip)
        if hit:
            start, name = hit
            if name.startswith("JIT_0x"):
                guest = name[4:].split("_", 1)[0]
                return "fex-jit", "jit:outside-images", guest
            if name.startswith("Dispatch_"):
                return "fex-dispatcher", "dispatcher", "+0x%x" % (ip - start)
            m = BLOCK.match(name)
            if m:
                return "fex-jit", "jit:" + winbase(m.group(1)), "+0x%s" % m.group(2).lower()
            return "fex-jit", "jit:" + winbase(name), None
        if self.perfmap.in_space(ip):
            return "fex-jit", "jit:no-symbol", None
        found = self.mapping(ip)
        if not found:
            img = self.pe_image(ip)
            if img:
                return self.pe_hit(ip, img)
            return "unknown", "unknown", "0x%x" % ip
        lo, hi, (perms, off, name) = found
        if name == "[anon:FEXMemJIT]":
            return "fex-jit", "jit:no-symbol", None
        if name == "[anon:FEXMem_Misc]":
            return "fex-dispatcher", "dispatcher", "+0x%x" % (ip - lo)
        if name.startswith("[anon:FEXMem"):
            return "fex-other", name, None
        if name == "[vdso]":
            return "vdso", "vdso", None
        if name.startswith("/"):
            cat = classify_file(name)
            base = self.image_base.get(name)
            if base is not None and name.lower().endswith(PE_EXT):
                loc = "rva 0x%x" % (ip - base)
            else:
                loc = "off 0x%x" % (ip - lo + off)
            return cat, os.path.basename(name), loc
        img = self.pe_image(ip)
        if img:
            return self.pe_hit(ip, img)
        if "x" in perms and self.named_run and (not name or name == "//anon"):
            return "fex-jit", "jit:no-symbol", None
        return "anon-exec", name or "[anon]", "0x%x" % ip

    def pe_hit(self, ip, hit):
        lo, _, img = hit
        name = img.get("name") or os.path.basename(img.get("file") or "") or "pe@0x%x" % lo
        cat = classify_file(img["file"]) if img.get("file") else classify_file(name)
        if cat == "other-pe" and not img.get("file"):
            cat = "wine-pe"
        return cat, name.lower(), "rva 0x%x" % (ip - lo)


def thread_cpu(first, last, tid):
    if not (first and last and first.tasks and last.tasks) or first is last:
        return None
    a = {t["tid"]: t for t in first.tasks["threads"]}
    b = {t["tid"]: t for t in last.tasks["threads"]}
    if tid not in a or tid not in b:
        return None
    dt = last.tasks["time"] - first.tasks["time"]
    hz = float(last.tasks.get("clk_tck") or 100)
    if dt <= 0:
        return None
    u = (b[tid]["utime"] - a[tid]["utime"]) / hz / dt
    s = (b[tid]["stime"] - a[tid]["stime"]) / hz / dt
    return {"dt": round(dt, 1), "busy": round(u + s, 3), "user": round(u, 3), "system": round(s, 3)}


def process_cpu(first, last):
    """Whole-process CPU over the snapshots: the game, wineserver, the tracer (proot)."""
    if not (first and last and first.tasks and last.tasks) or first is last:
        return []
    a = {p["pid"]: p for p in first.tasks.get("procs", [])}
    dt = last.tasks["time"] - first.tasks["time"]
    hz = float(last.tasks.get("clk_tck") or 100)
    out = []
    for p in last.tasks.get("procs", []):
        q = a.get(p["pid"])
        if q and dt > 0:
            out.append({"pid": p["pid"], "comm": p["comm"], "cpus": round((p["utime"] + p["stime"] - q["utime"] - q["stime"]) / hz / dt, 3),
                        "system": round((p["stime"] - q["stime"]) / hz / dt, 3)})
    return out


def report(dump, snapdirs, tids=None, min_pct=1.0, top=15):
    snaps = [Snapshot(d) for d in snapdirs]
    samples, mmaps, comms = parse_dump(dump)
    if not samples:
        raise ValueError("no samples in %s" % dump)
    ids = next((s.ids for s in reversed(snaps) if s.ids), None) or {}
    per_tid = Counter(s[1] for s in samples)
    pid_of = {s[1]: s[0] for s in samples}
    main = ids.get("main_tid")
    if tids:
        chosen = [t for t in tids if t in per_tid]
    else:
        total = sum(per_tid.values())
        chosen = [t for t, n in per_tid.most_common() if n * 100.0 / total >= min_pct]
        if main in per_tid and main not in chosen:
            chosen.insert(0, main)
        chosen.sort(key=lambda t: (t != main, -per_tid[t]))
    resolvers = {}
    names = {}
    for s in snaps:
        for t in (s.tasks or {}).get("threads", []):
            names[t["tid"]] = t["comm"]
    names.update(comms)
    times = [s[5] for s in samples if s[5] is not None]
    span = (max(times) - min(times)) / 1e9 if len(times) > 1 else None
    out = {"dump": dump, "snapshots": snapdirs, "samples": len(samples), "span_s": round(span, 2) if span else None,
           "pid": ids.get("pid"), "main_tid": main, "threads": []}
    jit_total = jit_named = 0
    for tid in chosen:
        pid = pid_of[tid]
        if pid not in resolvers:
            resolvers[pid] = Resolver(snaps if pid == ids.get("pid", pid) else [], mmaps, pid)
        r = resolvers[pid]
        cats, mods, locs = Counter(), Counter(), Counter()
        period = 0
        n = 0
        for spid, stid, ip, kernel, per, _ in samples:
            if stid != tid:
                continue
            cat, mod, loc = r.resolve(ip, kernel)
            cats[cat] += 1
            mods[(cat, mod)] += 1
            if loc:
                locs[(cat, mod, loc)] += 1
            period += per
            n += 1
            if cat == "fex-jit":
                jit_total += 1
                jit_named += mod != "jit:no-symbol"
        row = {"tid": tid, "comm": names.get(tid), "main": tid == main, "samples": n,
               "period_ms_per_s": round(period / 1e6 / span, 1) if span else None,
               "cpu": thread_cpu(snaps[0] if snaps else None, snaps[-1] if snaps else None, tid),
               "categories": [], "top": []}
        for cat in sorted(cats, key=lambda c: (ORDER.index(c) if c in ORDER else len(ORDER), -cats[c])):
            subs = sorted(((m, k) for (c, m), k in mods.items() if c == cat), key=lambda x: -x[1])
            row["categories"].append({"category": cat, "samples": cats[cat], "pct": round(cats[cat] * 100.0 / n, 2),
                                      "modules": [{"module": m, "samples": k, "pct": round(k * 100.0 / n, 2)} for m, k in subs]})
        for (cat, mod, loc), k in locs.most_common(top):
            row["top"].append({"category": cat, "module": mod, "location": loc, "samples": k, "pct": round(k * 100.0 / n, 2)})
        out["threads"].append(row)
    game = resolvers.get(ids.get("pid")) or next(iter(resolvers.values()))
    out["perfmap"] = {"file": next((s.perfmap for s in reversed(snaps) if s.perfmap), None), "lines": game.perfmap.lines,
                      "meta": next((s.perfmap_meta for s in reversed(snaps) if s.perfmap_meta), None),
                      "jit_samples": jit_total, "jit_with_symbol_pct": round(jit_named * 100.0 / jit_total, 1) if jit_total else None}
    env = next((s.env for s in reversed(snaps) if s.env), None) or {}
    out["env"] = {k: v for k, v in sorted(env.items()) if k.startswith(("FEX_", "FEXPROF_"))}
    out["processes"] = process_cpu(snaps[0] if snaps else None, snaps[-1] if snaps else None)
    fa = next((s.fex for s in snaps if s.fex), None)
    fb = next((s.fex for s in reversed(snaps) if s.fex), None)
    out["fexstats"] = fexstats.rates(fa, fb) if fa and fb and fa is not fb else None
    return out


def show(res):
    print("%d samples over %s s, pid %s, main thread %s" % (res["samples"], res["span_s"], res["pid"], res["main_tid"]))
    if res["env"]:
        print("game FEX env: " + " ".join("%s=%s" % kv for kv in res["env"].items()))
    pm = res["perfmap"]
    print("FEX symbol file: %s (%d lines); fex-jit samples with a block symbol: %s%%" % (
        pm["file"] or "none", pm["lines"], pm["jit_with_symbol_pct"] if pm["jit_with_symbol_pct"] is not None else "-"))
    for t in res["threads"]:
        cpu = t["cpu"]
        print("\nthread %d (%s)%s: %d samples%s%s" % (
            t["tid"], t["comm"], " [main]" if t["main"] else "", t["samples"],
            ", %.0f ms/s on-cpu by sample period" % t["period_ms_per_s"] if t["period_ms_per_s"] else "",
            ", /proc: busy %.1f%% (user %.1f%%, system %.1f%%) over %.0f s" % (cpu["busy"] * 100, cpu["user"] * 100, cpu["system"] * 100, cpu["dt"]) if cpu else ""))
        for c in t["categories"]:
            print("  %-16s %6.2f%%  %d" % (c["category"], c["pct"], c["samples"]))
            if len(c["modules"]) > 1 or c["modules"][0]["module"] != c["category"]:
                for m in c["modules"][:12]:
                    print("      %-34s %6.2f%%" % (m["module"][:34], m["pct"]))
        if t["top"]:
            print("  top locations:")
            for x in t["top"]:
                print("    %6.2f%%  %-14s %s %s" % (x["pct"], x["category"], x["module"], x["location"]))
    if res.get("processes"):
        print("\nprocess CPU between the snapshots (CPUs busy, of which kernel):")
        for p in res["processes"]:
            print("  %7d %-16s %.3f (%.3f)" % (p["pid"], p["comm"], p["cpus"], p["system"]))
    fx = res["fexstats"]
    if fx:
        print()
        fexstats.show(fx)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dump")
    ap.add_argument("snapdirs", nargs="*")
    ap.add_argument("--tid", help="comma-separated thread ids (default: the main thread and every thread with >= --min-pct of the samples)")
    ap.add_argument("--min-pct", type=float, default=1.0)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--json", help="also write the result as JSON to this file")
    args = ap.parse_args(argv)
    tids = [int(x) for x in args.tid.split(",")] if args.tid else None
    try:
        res = report(args.dump, args.snapdirs, tids, args.min_pct, args.top)
    except ValueError as e:
        print("prof-report: %s" % e, file=sys.stderr)
        return 1
    show(res)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(res, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
