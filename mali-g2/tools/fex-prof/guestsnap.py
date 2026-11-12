#!/usr/bin/env python3
"""guestsnap.py: what the profile report needs from inside the guest about a running game process.

usage: guestsnap.py ids [--comm Duckov.exe] [--pid PID]
       guestsnap.py snap DIR [--comm Duckov.exe] [--pid PID] [--no-pe] [--no-perfmap]

ids   prints the game process (the one named --comm with the most threads), its main thread (tid == pid: Wine runs
      the Windows main thread on the process's first thread), the wineserver processes and the process tracing it
      (proot), one "KEY value" per line.
snap  writes into DIR: maps.txt (/proc/PID/maps), tasks.json (per thread comm, state, cpu, utime/stime ticks, with a
      CLOCK_MONOTONIC time), ids.json, env.json (the game's FEX_*, WINE*, DXVK_* and Proton variables), fexstats.json
      (fexstats.py snapshot, if FEX keeps stats), pe.json (PE images found at mapping starts through /proc/PID/mem:
      base, SizeOfImage, machine, export name, mapped file; finds images Wine read into anonymous memory; only
      anonymous and regular-file mappings are read, never device or dma-buf ones) and
      perfmap.txt.gz + perfmap.json (FEX's JIT symbol file when FEX_BLOCKJITNAMING/LIBRARYJITNAMING/GLOBALJITNAMING
      is on). FEX's Windows build opens "/tmp/perf-<unix pid>.map" through the C runtime, so Wine resolves it on the
      process's current drive: Z: gives the guest's /tmp, C: the prefix's drive_c/tmp; every drive of the prefix is
      searched.
"""
import argparse
import glob
import gzip
import json
import os
import shutil
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fexstats  # noqa: E402

ENV_PREFIXES = ("FEX_", "WINE", "DXVK_", "PROTON_", "STEAM_COMPAT_DATA_PATH", "SteamGameId", "LD_PRELOAD")


def save(path, obj, indent=None):
    with open(path, "w") as f:
        json.dump(obj, f, indent=indent)


def read(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def comm_of(pid, tid=None):
    try:
        return read("/proc/%d/task/%d/comm" % (pid, tid) if tid else "/proc/%d/comm" % pid).strip()
    except OSError:
        return None


def processes(name):
    out = []
    for c in glob.glob("/proc/[0-9]*/comm"):
        try:
            if read(c).strip() != name:
                continue
            pid = int(c.split("/")[2])
            out.append((len(os.listdir("/proc/%d/task" % pid)), pid))
        except (OSError, ValueError):
            continue
    return [p for _, p in sorted(out, reverse=True)]


def status(pid):
    d = {}
    try:
        for line in read("/proc/%d/status" % pid).splitlines():
            k, _, v = line.partition(":")
            d[k] = v.strip()
    except OSError:
        pass
    return d


def ids(pid=None, comm="Duckov.exe"):
    if pid is None:
        found = processes(comm)
        if not found:
            return None
        pid = found[0]
    st = status(pid)
    tracer = int(st.get("TracerPid", "0") or 0)
    return {"pid": pid, "comm": comm_of(pid), "main_tid": pid, "threads": len(os.listdir("/proc/%d/task" % pid)),
            "wineserver": processes("wineserver"), "tracer": tracer, "tracer_comm": comm_of(tracer) if tracer else None}


def stat_row(path):
    stat = read(path)
    rest = stat.rsplit(")", 1)[1].split()
    return {"comm": stat[stat.index("(") + 1:stat.rindex(")")], "state": rest[0], "utime": int(rest[11]),
            "stime": int(rest[12]), "cpu": int(rest[36])}


def tasks(pid, others=()):
    """Per-thread CPU ticks of pid, and whole-process ticks of pid and of others (wineserver, the tracer)."""
    rows, procs = [], []
    for t in sorted(os.listdir("/proc/%d/task" % pid), key=int):
        try:
            rows.append(dict(tid=int(t), **stat_row("/proc/%d/task/%s/stat" % (pid, t))))
        except OSError:
            continue
    for p in [pid] + [o for o in others if o and o != pid]:
        try:
            procs.append(dict(pid=p, **stat_row("/proc/%d/stat" % p)))
        except OSError:
            continue
    return {"time": time.monotonic(), "clk_tck": os.sysconf("SC_CLK_TCK"), "threads": rows, "procs": procs}


def maps_lines(pid):
    rows = []
    for line in read("/proc/%d/maps" % pid).splitlines():
        parts = line.split(None, 5)
        if len(parts) < 5:
            continue
        lo, hi = (int(x, 16) for x in parts[0].split("-"))
        rows.append((lo, hi, parts[1], int(parts[2], 16), parts[5] if len(parts) > 5 else ""))
    return rows


def scannable(name):
    """Anonymous memory and regular files; not device or dma-buf mappings (GPU memory, doorbells) and not FEX's own."""
    if name == "":
        return True
    if name.startswith("[anon:"):
        return not name.startswith("[anon:FEX")
    return name.startswith("/") and not name.startswith(("/dev/", "/dmabuf"))


def pe_scan(pid, rows):
    """PE images at mapping starts: [{base, size, machine, name, file}]."""
    images = []
    try:
        mem = open("/proc/%d/mem" % pid, "rb", buffering=0)
    except OSError as e:
        return {"error": "open /proc/%d/mem: %s" % (pid, e), "images": []}

    def peek(addr, n):
        try:
            mem.seek(addr)
            return mem.read(n) or b""
        except (OSError, OverflowError, ValueError):
            return b""

    covered = 0
    with mem:
        for lo, hi, perms, _, name in rows:
            if lo < covered or perms[0] != "r" or not scannable(name):
                continue
            if peek(lo, 2) != b"MZ":
                continue
            head = peek(lo, 4096)
            if len(head) < 0x40:
                continue
            e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
            if e_lfanew + 24 + 112 > len(head) or head[e_lfanew:e_lfanew + 4] != b"PE\0\0":
                continue
            machine = struct.unpack_from("<H", head, e_lfanew + 4)[0]
            opt = e_lfanew + 24
            magic = struct.unpack_from("<H", head, opt)[0]
            if magic not in (0x10B, 0x20B):
                continue
            size = struct.unpack_from("<I", head, opt + 56)[0]
            ddir = opt + (112 if magic == 0x20B else 96)
            export_rva = struct.unpack_from("<I", head, ddir)[0] if ddir + 8 <= len(head) else 0
            export = ""
            if export_rva and export_rva < size:
                ed = peek(lo + export_rva, 40)
                if len(ed) == 40:
                    name_rva = struct.unpack_from("<I", ed, 12)[0]
                    if name_rva and name_rva < size:
                        export = peek(lo + name_rva, 260).split(b"\0", 1)[0].decode("ascii", "replace")
            images.append({"base": lo, "size": size, "machine": "0x%04x" % machine, "pe32plus": magic == 0x20B,
                           "name": export, "file": name if name.startswith("/") else ""})
            covered = lo + size
    return {"images": images}


def perfmap_candidates(pid, env):
    out = ["/tmp/perf-%d.map" % pid]
    prefix = env.get("WINEPREFIX") or (os.path.join(env["STEAM_COMPAT_DATA_PATH"], "pfx") if env.get("STEAM_COMPAT_DATA_PATH") else "")
    if prefix:
        out.append(os.path.join(prefix, "drive_c", "tmp", "perf-%d.map" % pid))
        for d in sorted(glob.glob(os.path.join(prefix, "dosdevices", "*:"))):
            out.append(os.path.join(os.path.realpath(d), "tmp", "perf-%d.map" % pid))
    seen, uniq = set(), []
    for p in out:
        r = os.path.realpath(p)
        if r not in seen:
            seen.add(r)
            uniq.append(p)
    return uniq


def environ(pid):
    try:
        raw = read("/proc/%d/environ" % pid, "rb").split(b"\0")
    except OSError as e:
        return {"error": str(e)}
    env = {}
    for kv in raw:
        k, sep, v = kv.decode("utf-8", "replace").partition("=")
        if sep and k.startswith(ENV_PREFIXES):
            env[k] = v
    return env


def snap(out, pid=None, comm="Duckov.exe", pe=True, perfmap=True):
    info = ids(pid, comm)
    if not info:
        print("GAME none (%s)" % comm)
        return 3
    pid = info["pid"]
    os.makedirs(out, exist_ok=True)
    t0 = time.monotonic()
    rows = maps_lines(pid)
    with open(os.path.join(out, "maps.txt"), "w") as f:
        f.write(read("/proc/%d/maps" % pid))
    save(os.path.join(out, "tasks.json"), tasks(pid, info["wineserver"] + [info["tracer"]]))
    env = environ(pid)
    save(os.path.join(out, "env.json"), env, indent=1)
    info["time"] = t0
    save(os.path.join(out, "ids.json"), info, indent=1)
    try:
        save(os.path.join(out, "fexstats.json"), fexstats.snapshot(pid, freq=fexstats.cntfrq()))
        stats = "yes"
    except (OSError, ValueError) as e:
        stats = "no (%s)" % e
    if pe:
        res = pe_scan(pid, rows)
        save(os.path.join(out, "pe.json"), res, indent=1)
        pe_note = "%d images%s" % (len(res["images"]), " (%s)" % res["error"] if res.get("error") else "")
    else:
        pe_note = "skipped"
    pm_note = "skipped"
    if perfmap:
        found = [(os.path.getsize(p), p) for p in perfmap_candidates(pid, env) if os.path.isfile(p)]
        meta = {"candidates": perfmap_candidates(pid, env), "found": [{"path": p, "size": s} for s, p in found]}
        if found:
            size, path = max(found)
            with open(path, "rb") as src, gzip.open(os.path.join(out, "perfmap.txt.gz"), "wb") as dst:
                shutil.copyfileobj(src, dst)
            meta["copied"] = path
            pm_note = "%s (%d bytes)" % (path, size)
        else:
            pm_note = "none"
        save(os.path.join(out, "perfmap.json"), meta, indent=1)
    print("SNAP %s pid %d (%s, %d threads), %d mappings, fex stats %s, pe %s, perf map %s, %.1f s" % (
        out, pid, info["comm"], info["threads"], len(rows), stats, pe_note, pm_note, time.monotonic() - t0))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("ids", "snap"):
        p = sub.add_parser(name)
        if name == "snap":
            p.add_argument("dir")
            p.add_argument("--no-pe", action="store_true")
            p.add_argument("--no-perfmap", action="store_true")
        p.add_argument("--comm", default=os.environ.get("GAME_COMM", "Duckov.exe"))
        p.add_argument("--pid", type=int)
    args = ap.parse_args(argv)
    if args.cmd == "ids":
        info = ids(args.pid, args.comm)
        if not info:
            print("GAME none (%s)" % args.comm)
            return 3
        for k, v in info.items():
            print("%s %s" % (k.upper(), ",".join(map(str, v)) if isinstance(v, list) else v))
        return 0
    return snap(args.dir, args.pid, args.comm, not args.no_pe, not args.no_perfmap)


if __name__ == "__main__":
    sys.exit(main())
