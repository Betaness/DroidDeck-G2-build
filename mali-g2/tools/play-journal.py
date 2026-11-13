#!/usr/bin/env python3
"""DroidDeck G2's play journal: one record per game played, read over adb (no root).

usage: play-journal.py [--file PLAYS.jsonl] [--package PKG] [--json] COMMAND ...
  list [-n 30] [--game TEXT] [--since 2d]   one row per play, newest last (the default command)
  games                                      per game: plays, time, typical fps, worst lows, problems, stream output
  show <#|id|last>                           one play in full: per-minute fps / Hz / temperatures, errors, env
  problems                                   plays with flags and the owner's marks, and whether their logs are still there
  open                                       plays in progress (or left by an app that died: recorded at the next session)
  pull <#|id|last> [DEST]                    adb pull the play's session folder (files/logs/<folder>)

The app writes getExternalFilesDir()/journal/plays.jsonl (/sdcard/Android/data/<pkg>/files/journal/), one JSON object
per line: "play" records (see PlayRecord.kt for the fields) and "mark" records ("Something went wrong" in the
session drawer). The package defaults to $DROIDDECK_PACKAGE, else com.droiddeck.launcher.g2. --json prints the
selected records as JSON lines for further scripting. Lines that do not parse are skipped and counted.

fps are frames the game presented (its own layer) or, for a game drawn through X11 (copy path, a window paced as
hidden), frames the compositor composited: the list marks those with "s" (fps.source "screen"). Frame times
(1% low, p99) exist only for the game layer. Hz per minute is the time-weighted mean refresh rate.

gpu.xfb (only in plays that created a pipeline with D3D11 stream output or a geometry shader, mali-g2/vortek/src/
xfb_stats.h): pipelines with transform feedback, gs pipelines with a geometry shader, begins of transform feedback
(a lower bound of the stream-output draws), drawAuto, beginsPerMin. `games` shows the most of any play as
"<pipelines>p/<gs>g <begins per minute>/min" ("-": no play of the game recorded any).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timedelta

DEFAULT_PACKAGE = "com.droiddeck.launcher.g2"
SPARK = " .:-=+*#%@"
FLAG_SHORT = {
    "crash": "CRASH", "device_lost": "DEVLOST", "vortek_exit": "VORTEK", "app_killed": "KILLED", "launch_failed": "NOLAUNCH",
    "freeze": "freeze", "low_fps": "lowfps", "oom": "oom", "hidden_paced": "hidden", "compiler": "compiler",
    "at60_thermal": "60Hz-hot", "marked": "MARKED", "x11_copy": "x11", "software": "CPU-DRAWN", "no_sempool": "nopool",
}
SOURCE_TAG = {"game-layer": "", "screen": "s", "x11-stats": "x", "stats": "t", "x11": "x"}


def files_dir(package):
    return f"/sdcard/Android/data/{package}/files"


def adb(*args, binary=False):
    try:
        out = subprocess.run(["adb", *args], capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise SystemExit(f"adb failed: {e}")
    if out.returncode != 0:
        raise SystemExit(f"adb {' '.join(args)}: {out.stderr.decode(errors='replace').strip() or out.returncode}")
    return out.stdout if binary else out.stdout.decode("utf-8", errors="replace")


def parse_lines(text):
    """(records, skipped): every line that is a JSON object; a partial last line counts as skipped."""
    records, skipped = [], 0
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except ValueError:
            skipped += 1
            continue
        if isinstance(r, dict):
            records.append(r)
        else:
            skipped += 1
    return records, skipped


def load(args, name="plays.jsonl"):
    if args.file:
        path = args.file if name == "plays.jsonl" else os.path.join(os.path.dirname(args.file) or ".", name)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            if name == "plays.jsonl":
                raise SystemExit(f"{path}: no such file")
            return ""
    out = subprocess.run(["adb", "exec-out", "cat", f"{files_dir(args.package)}/journal/{name}"], capture_output=True)
    if out.returncode != 0 or out.stdout.startswith(b"cat: "):
        if name == "plays.jsonl":
            msg = (out.stdout or out.stderr).decode(errors="replace").strip()
            raise SystemExit(f"no journal on the device yet ({msg or 'adb failed'})")
        return ""
    return out.stdout.decode("utf-8", errors="replace")


def stamp(iso):
    """'2026-10-28T01:17:32+11:00' -> datetime (aware), or None."""
    if not iso:
        return None
    try:
        return datetime.fromisoformat(iso)
    except ValueError:
        return None


def short_time(iso):
    t = stamp(iso)
    return t.strftime("%m-%d %H:%M") if t else "?"


def duration(s):
    if s is None:
        return "?"
    s = int(s)
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m"
    return f"{s // 3600}h{(s % 3600) // 60:02d}"


def num(v, fmt="{:.0f}"):
    return "-" if v is None else fmt.format(v)


def get(r, *path):
    for p in path:
        if not isinstance(r, dict):
            return None
        r = r.get(p)
    return r


def sparkline(values, lo=None, hi=None):
    vals = [v for v in values if isinstance(v, (int, float))]
    if not vals:
        return ""
    lo = min(vals) if lo is None else lo
    hi = max(vals) if hi is None else hi
    span = hi - lo
    out = []
    for v in values:
        if not isinstance(v, (int, float)):
            out.append("_")
        elif span <= 0:
            out.append(SPARK[len(SPARK) // 2])
        else:
            out.append(SPARK[max(0, min(len(SPARK) - 1, int((v - lo) / span * (len(SPARK) - 1) + 0.5)))])
    return "".join(out)


def flags_text(r):
    return ",".join(FLAG_SHORT.get(f, f) for f in (r.get("flags") or []))


def plays_of(records):
    return [r for r in records if r.get("type") == "play"]


def since_cutoff(text):
    m = re.fullmatch(r"(\d+)([hdw])", text or "")
    if not m:
        raise SystemExit(f"--since: want e.g. 12h, 2d, 1w, not {text!r}")
    n, unit = int(m.group(1)), m.group(2)
    return time.time() - n * {"h": 3600, "d": 86400, "w": 7 * 86400}[unit]


def select(plays, game=None, since=None):
    out = []
    for i, r in enumerate(plays, 1):
        if game and game.lower() not in (str(r.get("game") or "") + " " + str(r.get("appid") or "")).lower():
            continue
        if since is not None:
            t = stamp(r.get("start"))
            if t is None or t.timestamp() < since:
                continue
        out.append((i, r))
    return out


def find(plays, key):
    if key in ("last", "-1"):
        return (len(plays), plays[-1]) if plays else None
    if key.lstrip("#").isdigit():
        i = int(key.lstrip("#"))
        return (i, plays[i - 1]) if 1 <= i <= len(plays) else None
    for i, r in enumerate(plays, 1):
        if r.get("id") == key:
            return i, r
    return None


def row(i, r):
    fps = r.get("fps") or {}
    hz = r.get("hz") or {}
    at60 = hz.get("at60")
    hot = get(hz, "why", "thermal")
    skin = get(r, "thermal", "skinC")
    batt = get(r, "thermal", "batteryC")
    skin_max = skin[1] if isinstance(skin, list) and len(skin) == 2 else None
    batt_max = batt[1] if isinstance(batt, list) and len(batt) == 2 else None
    mem = get(r, "mem", "gameHwmMB") or get(r, "mem", "gameRssMaxMB")
    mean = fps.get("mean")
    if mean is None:
        mean = fps.get("x11Fps") if fps.get("x11Fps") is not None else fps.get("statsFps", fps.get("screenFps"))
    tag = SOURCE_TAG.get(fps.get("source"), "?") if mean is not None else ""
    return [
        str(i), short_time(r.get("start")), str(r.get("game") or r.get("appid") or "?")[:24], duration(r.get("durS")),
        str(get(r, "exit", "reason") or "?"), str(get(r, "gpu", "used") or get(r, "gpu", "asked") or "?"),
        f"{num(mean)}{tag}/{num(fps.get('low1'))}", num(fps.get("ftP99"), "{:.1f}"),
        "-" if at60 is None else f"{at60 * 100:.0f}%" + (f"({(hot or 0) * 100:.0f})" if hot else ""),
        num(skin_max, "{:.1f}") if skin_max is not None else num(batt_max, "b{:.1f}"),
        num(mem), flags_text(r), str(r.get("logs") or ""),
    ]


HEADER = ["#", "start", "game", "dur", "exit", "gpu", "fps/1%", "p99ms", "60Hz(hot)", "skin", "memMB", "flags", "logs"]


def table(rows, header):
    widths = [max(len(header[c]), *(len(r[c]) for r in rows)) if rows else len(header[c]) for c in range(len(header))]
    lines = ["  ".join(h.ljust(w) for h, w in zip(header, widths)).rstrip()]
    for r in rows:
        lines.append("  ".join(v.ljust(w) for v, w in zip(r, widths)).rstrip())
    return "\n".join(lines)


def cmd_list(args, records):
    plays = plays_of(records)
    sel = select(plays, args.game, since_cutoff(args.since) if args.since else None)[-args.n:]
    if args.json:
        return "\n".join(json.dumps(r, ensure_ascii=False) for _, r in sel)
    if not sel:
        return "no plays recorded" + (" matching" if args.game or args.since else "")
    out = table([row(i, r) for i, r in sel], HEADER)
    marks = [m for m in records if m.get("type") == "mark"]
    if marks:
        out += f"\n{len(marks)} mark(s) by the owner: play-journal.py problems"
    return out


def median(vals):
    vals = sorted(vals)
    if not vals:
        return None
    n = len(vals)
    return vals[n // 2] if n % 2 else (vals[n // 2 - 1] + vals[n // 2]) / 2


def cmd_games(args, records):
    games = {}
    for r in plays_of(records):
        key = r.get("appid") or r.get("game") or "?"
        g = games.setdefault(key, {"name": r.get("game") or str(key), "plays": 0, "dur": 0, "focused": 0, "means": [], "low1": [],
                                    "crash": 0, "devlost": 0, "freeze": 0, "killed": 0, "last": None, "gpus": set(), "xfb": None})
        g["plays"] += 1
        g["dur"] += r.get("durS") or 0
        g["focused"] += r.get("focusedS") or 0
        mean = get(r, "fps", "meanAfter1m") or get(r, "fps", "mean")
        if mean is not None:
            g["means"].append(mean)
        if get(r, "fps", "low1") is not None:
            g["low1"].append(get(r, "fps", "low1"))
        flags = r.get("flags") or []
        g["crash"] += "crash" in flags or get(r, "exit", "reason") in ("crash", "vortek_exit")
        g["devlost"] += "device_lost" in flags
        g["freeze"] += "freeze" in flags
        g["killed"] += "app_killed" in flags
        g["last"] = r.get("start")
        used = get(r, "gpu", "used")
        if used:
            g["gpus"].add(used)
        xfb = get(r, "gpu", "xfb")
        if isinstance(xfb, dict):
            most = g["xfb"] or {"pipelines": 0, "gs": 0, "beginsPerMin": 0}
            g["xfb"] = {k: max(most[k], xfb.get(k) or 0) for k in most}
    if args.json:
        return "\n".join(json.dumps({**g, "appid": k, "gpus": sorted(g["gpus"])}, ensure_ascii=False) for k, g in games.items())
    rows = []
    for k, g in sorted(games.items(), key=lambda kv: kv[1]["last"] or ""):
        rows.append([str(g["name"])[:28], str(k), str(g["plays"]), duration(g["dur"]), duration(g["focused"]),
                     num(median(g["means"])), num(min(g["low1"]) if g["low1"] else None),
                     f"{g['crash']}/{g['devlost']}/{g['freeze']}/{g['killed']}", short_time(g["last"]), ",".join(sorted(g["gpus"])) or "-",
                     xfb_text(g["xfb"])])
    return table(rows, ["game", "appid", "plays", "time", "focused", "fps~", "worst1%", "crash/lost/freeze/killed", "last", "gpu", "xfb"])


def xfb_text(x):
    if not x:
        return "-"
    return f"{x['pipelines']}p/{x['gs']}g {num(x['beginsPerMin'])}/min"


def per_minute(r):
    fps = get(r, "fps", "perMin") or []
    hz = get(r, "hz", "perMin") or []
    skin = get(r, "thermal", "skinPerMin") or []
    batt = get(r, "thermal", "batteryPerMin") or []
    anon = get(r, "mem", "anonPerMin") or []
    n = max(len(fps), len(hz), len(skin), len(batt), len(anon))
    if not n:
        return ""

    def at(lst, i, fmt):
        return fmt.format(lst[i]) if i < len(lst) and lst[i] is not None else "-"

    lines = ["  min   fps    Hz  skinC  battC  anonMB"]
    for i in range(n):
        lines.append(f"  {i:3d}  {at(fps, i, '{:5.1f}'):>5}  {at(hz, i, '{:4d}'):>4}  {at(skin, i, '{:5.1f}'):>5}  {at(batt, i, '{:5.1f}'):>5}"
                     f"  {at(anon, i, '{:6d}'):>6}")
    lines.append(f"  fps  [{sparkline(fps, lo=0)}]")
    if any(isinstance(v, (int, float)) for v in hz):
        lines.append(f"  Hz   [{sparkline(hz, lo=0)}]")
    if any(isinstance(v, (int, float)) for v in skin):
        lines.append(f"  skin [{sparkline(skin)}]")
    if any(isinstance(v, (int, float)) for v in anon):
        lines.append(f"  anon [{sparkline(anon)}]")
    return "\n".join(lines)


def trend(series):
    """'first -> last MB, +x.x MB/min' over a per-minute list, or None with under two values."""
    pts = [(i, v) for i, v in enumerate(series or []) if isinstance(v, (int, float))]
    if len(pts) < 2:
        return None
    n = len(pts)
    mx = sum(i for i, _ in pts) / n
    my = sum(v for _, v in pts) / n
    var = sum((i - mx) ** 2 for i, _ in pts)
    slope = sum((i - mx) * (v - my) for i, v in pts) / var if var else 0.0
    return f"{pts[0][1]} -> {pts[-1][1]} MB, {slope:+.1f} MB/min over {pts[-1][0] - pts[0][0]} min"


def timeline_text(r):
    events = r.get("timeline") or []
    out = []
    for e in events:
        if not isinstance(e, list) or len(e) < 2:
            continue
        times = f" (x{e[2]})" if len(e) > 2 else ""
        out.append(f"  +{duration(e[0]):>6}  {e[1]}{times}")
    if r.get("timelineDropped"):
        out.append(f"  ({r['timelineDropped']} more not kept)")
    return "\n".join(out)


def kv(d, indent="  "):
    if not isinstance(d, dict) or not d:
        return f"{indent}-"
    width = max(len(str(k)) for k in d)
    return "\n".join(f"{indent}{str(k).ljust(width)}  {json.dumps(v, ensure_ascii=False) if not isinstance(v, str) else v}" for k, v in d.items())


def cmd_show(args, records):
    plays = plays_of(records)
    found = find(plays, args.key)
    if not found:
        raise SystemExit(f"no play {args.key!r} (there are {len(plays)})")
    i, r = found
    if args.json:
        return json.dumps(r, ensure_ascii=False, indent=1)
    ex = r.get("exit") or {}
    out = [
        f"#{i} {r.get('id')}  {r.get('game')} ({r.get('appid')})  {r.get('exe') or ''}",
        f"  {r.get('start')} .. {r.get('end')}  {duration(r.get('durS'))} (focused {duration(r.get('focusedS'))})"
        + (f"  relaunch of {r['relaunchOf']}" if r.get("relaunchOf") else "") + ("  [recorded late]" if r.get("recovered") else ""),
        f"  exit: {ex.get('reason')}" + "".join(f"  {k}={ex[k]}" for k in ("steamCode", "signal", "vortek") if ex.get(k) is not None)
        + (f"\n        {ex['detail']}" if ex.get("detail") else "")
        + (f"\n        app: {json.dumps(ex['appExit'], ensure_ascii=False)}" if ex.get("appExit") else ""),
        f"  flags: {', '.join(r.get('flags') or []) or '-'}",
        f"  logs: files/logs/{r.get('logs')}   app {get(r, 'app', 'version')} {get(r, 'app', 'commit')}",
        "gpu:", kv(r.get("gpu")),
        "sync:", kv({k: v for k, v in (r.get("env") or {}).items()
                     if k in ("BL_SYNC", "BL_SYNC_PACK", "BL_SYNC_SEMPOOL", "BL_SYNC_NO_PACK", "WINEESYNC", "BL_FSYNC",
                              "WINE_SEM_POOL")}),
        "display:", kv(r.get("display")),
        "fps:", kv({k: v for k, v in (r.get("fps") or {}).items() if k != "perMin"}),
        "refresh:", kv({k: v for k, v in (r.get("hz") or {}).items() if k != "perMin"}),
        "thermal:", kv({k: v for k, v in (r.get("thermal") or {}).items() if not k.endswith("PerMin")}),
        "cpu:", kv(r.get("cpu")),
        "mibridge (share granted):", kv(r.get("mibridge")),
        "memory:", kv({k: v for k, v in (r.get("mem") or {}).items() if not k.endswith("PerMin")}),
    ]
    for label, key in (("game anon", "anonPerMin"), ("vortek anon", "vortekAnonPerMin")):
        t = trend(get(r, "mem", key))
        if t:
            out.append(f"  {label}: {t}")
    out += ["per minute:", per_minute(r) or "  -"]
    tl = timeline_text(r)
    out.append("timeline:" + ("\n" + tl if tl else " -"))
    errors = r.get("errors") or []
    out.append("errors:" + ("" if errors else " -"))
    for e in errors:
        where = "" if e.get("game", True) else "  (session logs, not the game's)"
        at = f"  first at +{duration(e['atS'])}" if e.get("atS") is not None else ""
        out.append(f"  {e.get('k')} x{e.get('n')}{at}{where}")
        for line in e.get("first") or []:
            out.append(f"      {line}")
    if r.get("scanned"):
        out.append(f"scanned: {', '.join(r['scanned'])}")
    marks = r.get("marks") or []
    out.append("marks: " + (", ".join(f"{m.get('t')} (+{duration(m.get('atS'))})" for m in marks) or "-"))
    out += ["settings:", kv(r.get("settings")), "env:", kv(r.get("env"))]
    return "\n".join(out)


def logs_present(args, names):
    """Which session folders are still mirrored, or None when it cannot be told."""
    if args.logs_dir:
        return {n for n in names if os.path.isdir(os.path.join(args.logs_dir, n))}
    if args.file:
        return None
    try:
        listing = adb("shell", "ls", f"{files_dir(args.package)}/logs")
    except SystemExit:
        return None
    return set(listing.split()) & set(names)


def cmd_problems(args, records):
    plays = plays_of(records)
    bad = [(i, r) for i, r in enumerate(plays, 1) if r.get("flags")]
    marks = [m for m in records if m.get("type") == "mark"]
    if args.json:
        return "\n".join(json.dumps(r, ensure_ascii=False) for r in [r for _, r in bad] + marks)
    present = logs_present(args, {r.get("logs") for _, r in bad} | {m.get("logs") for m in marks})

    def have(name):
        return "?" if present is None else ("yes" if name in present else "gone")

    out = []
    if bad:
        rows = [row(i, r)[:5] + [flags_text(r), str(get(r, "exit", "detail") or "")[:60], str(r.get("logs")), have(r.get("logs"))] for i, r in bad]
        out.append(table(rows, HEADER[:5] + ["flags", "detail", "logs", "kept"]))
    else:
        out.append("no flagged plays")
    if marks:
        out.append("")
        rows = [[short_time(m.get("t")), str(m.get("game") or "-")[:24], duration(m.get("playS")) if m.get("playS") is not None else "-",
                 sparkline(m.get("fpsLast60s") or [], lo=0), num(m.get("modeHz")), str(m.get("thermalStatus") or "-"),
                 num(m.get("skinC"), "{:.1f}"), str(m.get("logs")), have(m.get("logs"))] for m in marks]
        out.append(table(rows, ["marked", "game", "into", "fps last 60s", "Hz", "thermal", "skin", "logs", "kept"]))
    return "\n".join(out)


def cmd_open(args, records):
    text = load(args, "open.json")
    if not text.strip():
        return "no plays in progress"
    try:
        state = json.loads(text)
    except ValueError:
        return "open.json does not parse"
    if args.json:
        return json.dumps(state, ensure_ascii=False, indent=1)
    out = [f"written {state.get('written')} by app pid {state.get('appPid')} (session {state.get('logs')})"]
    rows = [row(n, p.get("record") or {}) for n, p in enumerate(state.get("plays") or [], 1)]
    if rows:
        out.append(table(rows, HEADER))
    return "\n".join(out)


def cmd_pull(args, records):
    plays = plays_of(records)
    found = find(plays, args.key)
    if not found:
        raise SystemExit(f"no play {args.key!r}")
    _, r = found
    folder = r.get("logs")
    dest = args.dest or "."
    print(f"pulling files/logs/{folder} to {dest}")
    adb("pull", f"{files_dir(args.package)}/logs/{folder}", dest)
    return ""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", help="read this plays.jsonl instead of the device's")
    ap.add_argument("--package", default=os.environ.get("DROIDDECK_PACKAGE", DEFAULT_PACKAGE))
    ap.add_argument("--logs-dir", help="local copy of files/logs, to tell which session folders are kept")
    ap.add_argument("--json", action="store_true", help="the selected records as JSON")
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("list")
    p.add_argument("-n", type=int, default=30)
    p.add_argument("--game")
    p.add_argument("--since")
    sub.add_parser("games")
    p = sub.add_parser("show")
    p.add_argument("key")
    sub.add_parser("problems")
    sub.add_parser("open")
    p = sub.add_parser("pull")
    p.add_argument("key")
    p.add_argument("dest", nargs="?")
    args = ap.parse_args(argv)
    if args.cmd is None:
        args = ap.parse_args(list(sys.argv[1:] if argv is None else argv) + ["list"])
    records, skipped = parse_lines(load(args)) if args.cmd != "open" else ([], 0)
    out = {"list": cmd_list, "games": cmd_games, "show": cmd_show, "problems": cmd_problems, "open": cmd_open, "pull": cmd_pull}[args.cmd](args, records)
    if out:
        print(out)
    if skipped and not args.json:
        print(f"({skipped} line(s) skipped: not JSON)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
