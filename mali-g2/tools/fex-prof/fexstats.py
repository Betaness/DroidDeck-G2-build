#!/usr/bin/env python3
"""fexstats.py: FEX's live per-thread statistics (ProfileStats=1, which Proton's FEX config sets) of a process.

usage: fexstats.py snap PID [--file F]                      one snapshot as JSON
       fexstats.py rate PID [--seconds S] [--json]          two snapshots S seconds apart (default 10), per-thread rates
       fexstats.py diff A.json B.json [--json]              rates between two saved snapshots

FEX (eff9f459d, FEXCore/include/FEXCore/Utils/SHMStats.h, Source/Windows/UnixLib/FEXUnixLib.cpp) keeps the stats in
the POSIX shared memory object fex-<unix pid>-stats, i.e. /dev/shm/fex-<pid>-stats as the process itself sees it, so
run this in the guest. Layout (STATS_VERSION 2): a 64-byte header (u8 Version, u8 app_type, u16 ThreadStatsSize,
char fex_version[48], u32 Head, u32 Size, u32 Pad) and ThreadStatsSize-byte slots right after it; a slot with TID 0
is free. ARM64EC/WoW64 slots carry the Windows thread id (GetCurrentThreadId), not the unix tid; the process's first
thread takes the first slot, so slot 0 is the main thread while it lives. Times are CNTVCT_EL0 ticks: the counter
frequency is read here (cntfrq_el0) unless --cntfrq is given. A process whose FEX has no stats (ProfileStats=0, or
not a FEX process) has no file.
"""
import argparse
import json
import mmap
import os
import struct
import sys
import time

HEADER = struct.Struct("<BBH48sIII")
SLOT_HEAD = struct.Struct("<II")
COUNTERS = (
    ("jit_time", "t"),
    ("signal_time", "t"),
    ("sigbus", "n"),
    ("smc", "n"),
    ("float_fallback", "n"),
    ("cache_miss", "n"),
    ("cache_read_lock_time", "t"),
    ("cache_write_lock_time", "t"),
    ("jit_count", "n"),
    ("disk_cache_hit", "n"),
    ("disk_cache_miss", "n"),
    ("disk_cache_lookup_time", "t"),
)
APP_TYPES = {0: "linux32", 1: "linux64", 2: "win-arm64ec", 3: "win-wow64"}


def cntfrq():
    """CNTFRQ_EL0 of this CPU via a two-instruction function, or None where that cannot run (not aarch64, no
    executable anonymous memory, no libgcc_s to synchronise the instruction cache)."""
    if os.uname().machine != "aarch64":
        return None
    try:
        import ctypes
        clear = ctypes.CDLL("libgcc_s.so.1").__clear_cache
        page = mmap.mmap(-1, mmap.PAGESIZE, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    except (OSError, ValueError, ImportError, AttributeError):
        return None
    page.write(struct.pack("<II", 0xD53BE000, 0xD65F03C0))  # mrs x0, cntfrq_el0; ret
    addr = ctypes.addressof(ctypes.c_char.from_buffer(page))
    clear(ctypes.c_void_p(addr), ctypes.c_void_p(addr + 8))
    value = ctypes.CFUNCTYPE(ctypes.c_uint64)(addr)()
    return int(value) or None


def parse(data):
    """(header dict, [slot dict]) from the bytes of a stats file."""
    if len(data) < HEADER.size:
        raise ValueError("stats file shorter than its header (%d bytes)" % len(data))
    version, app, slot_size, ver, head, size, _ = HEADER.unpack_from(data, 0)
    header = {"version": version, "app_type": APP_TYPES.get(app, app), "slot_size": slot_size,
              "fex_version": ver.split(b"\0", 1)[0].decode("ascii", "replace"), "head": head, "size": size}
    need = SLOT_HEAD.size + 8 * len(COUNTERS)
    if slot_size < need:
        raise ValueError("slot size %d is smaller than the %d bytes this reader knows (stats version %d)" % (slot_size, need, version))
    slots = []
    count = (min(size, len(data)) - HEADER.size) // slot_size
    for i in range(count):
        off = HEADER.size + i * slot_size
        nxt, tid = SLOT_HEAD.unpack_from(data, off)
        if tid == 0:
            continue
        values = struct.unpack_from("<%dQ" % len(COUNTERS), data, off + SLOT_HEAD.size)
        slot = {"slot": i, "tid": tid}
        slot.update({name: v for (name, _), v in zip(COUNTERS, values)})
        slots.append(slot)
    return header, slots


def snapshot(pid, path=None, freq=None):
    path = path or "/dev/shm/fex-%d-stats" % pid
    with open(path, "rb") as f:
        data = f.read()
    t = time.monotonic()
    header, slots = parse(data)
    return {"pid": pid, "file": path, "time": t, "cntfrq": freq, "header": header, "threads": slots}


def rates(a, b, freq=None):
    """Per-slot rates between snapshots a and b (same process). Times in ms per second of wall time."""
    dt = b["time"] - a["time"]
    if dt <= 0:
        raise ValueError("snapshots are not in order (dt %.3f s)" % dt)
    freq = freq or b.get("cntfrq") or a.get("cntfrq")
    before = {(s["slot"], s["tid"]): s for s in a["threads"]}
    rows = []
    for s in b["threads"]:
        p = before.get((s["slot"], s["tid"]))
        row = {"slot": s["slot"], "tid": s["tid"], "new": p is None}
        for name, kind in COUNTERS:
            d = s[name] - (p[name] if p else 0)
            if kind == "t":
                row[name + "_ms_s"] = round(d / freq * 1000.0 / dt, 3) if freq else None
                row[name + "_ticks_s"] = round(d / dt, 1)
            else:
                row[name + "_s"] = round(d / dt, 2)
        rows.append(row)
    rows.sort(key=lambda r: r["slot"])
    return {"pid": b["pid"], "dt": round(dt, 3), "cntfrq": freq, "fex_version": b["header"]["fex_version"],
            "app_type": b["header"]["app_type"], "threads": rows}


def show(result):
    freq = result["cntfrq"]
    print("FEX %s (%s), pid %s, %.1f s, counter %s" % (result["fex_version"], result["app_type"], result["pid"], result["dt"],
                                                       "%.0f Hz" % freq if freq else "frequency unknown: times in ticks/s"))
    unit = "ms/s" if freq else "ticks/s"
    cols = [("jit_time", "jit " + unit), ("jit_count", "jits/s"), ("signal_time", "sig " + unit), ("sigbus", "sigbus/s"),
            ("smc", "smc/s"), ("float_fallback", "x87fb/s"), ("cache_miss", "miss/s"), ("cache_read_lock_time", "rdlk " + unit),
            ("cache_write_lock_time", "wrlk " + unit), ("disk_cache_hit", "dch/s"), ("disk_cache_miss", "dcm/s"),
            ("disk_cache_lookup_time", "dcl " + unit)]
    kinds = dict(COUNTERS)
    print("%5s %8s " % ("slot", "wintid") + " ".join("%11s" % c[1] for c in cols))
    for r in result["threads"]:
        vals = []
        for name, _ in cols:
            key = name + ("_ms_s" if freq else "_ticks_s") if kinds[name] == "t" else name + "_s"
            v = r.get(key)
            vals.append("%11s" % ("-" if v is None else v))
        print("%5d %8d " % (r["slot"], r["tid"]) + " ".join(vals) + ("  (new)" if r["new"] else ""))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snap")
    s.add_argument("pid", type=int)
    s.add_argument("--file")
    s.add_argument("--cntfrq", type=float)
    r = sub.add_parser("rate")
    r.add_argument("pid", type=int)
    r.add_argument("--file")
    r.add_argument("--seconds", type=float, default=10.0)
    r.add_argument("--cntfrq", type=float)
    r.add_argument("--json", action="store_true")
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--cntfrq", type=float)
    d.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    freq = args.cntfrq or (cntfrq() if args.cmd != "diff" else None)
    try:
        if args.cmd == "snap":
            json.dump(snapshot(args.pid, args.file, freq), sys.stdout)
            print()
            return 0
        if args.cmd == "rate":
            a = snapshot(args.pid, args.file, freq)
            time.sleep(args.seconds)
            res = rates(a, snapshot(args.pid, args.file, freq), freq)
        else:
            res = rates(json.load(open(args.a)), json.load(open(args.b)), freq)
    except FileNotFoundError as e:
        print("fexstats: no stats file (%s): ProfileStats off, or not a FEX process" % e.filename, file=sys.stderr)
        return 1
    except ValueError as e:
        print("fexstats: %s" % e, file=sys.stderr)
        return 1
    if args.json:
        json.dump(res, sys.stdout)
        print()
    else:
        show(res)
    return 0


if __name__ == "__main__":
    sys.exit(main())
