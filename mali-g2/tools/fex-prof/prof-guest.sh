# prof-guest.sh (runs in the DroidDeck guest: droiddeckctl guest, or a test harness's g.sh) <command> [args]
# The guest half of the main-thread profile (README.md): FEX's JIT symbol file, the process's mappings, threads and
# FEX stats around a simpleperf recording that adb starts outside the guest.
#   install            copy guestsnap.py, fexstats.py and prof-report.py from $INBOX (adb push them there first) to $Q
#   env set [MODE]     before the game starts (the app rewrites game-environment.json at every session start):
#                      games[$APPID] gets MODE's FEX settings. block (default): FEX_BLOCKJITNAMING=1 (one symbol per
#                      compiled block: module+offset, or the guest address for code outside any PE image such as
#                      Mono's JIT) and FEX_DISKCACHE=0 (blocks loaded from FEX's disk cache get no symbol); library:
#                      FEX_LIBRARYJITNAMING=1 and FEX_DISKCACHE=0; none: only the marker. Every mode also sets
#                      FEXPROF_MODE=<mode> so the snapshot shows which run it was. Also creates the prefix's C:\tmp
#                      (FEX writes \tmp\perf-<pid>.map on the current drive) and the marker $Q/env-set.
#   env unset          take those entries out again (a running game keeps them until it exits)
#   ids                the game process, main thread, wineserver and tracer (guestsnap.py ids)
#   snap TAG [ARGS]    guestsnap.py snap $Q/TAG (maps, threads, env, FEX stats, PE images, FEX's symbol file)
#   stats [S]          fexstats.py rate over S seconds (default 10) for the game
#   pack TAG           $Q/TAG as $OUTBOX/fexprof-TAG.tar.gz, for adb pull
#   restore            env unset; delete the perf-*.map files newer than $Q/env-set in $GUEST_TMP and in each drive's
#                      \tmp (every FEX process of the game's environment writes one) and C:\tmp if env set made
#                      it; delete $Q and $OUTBOX/fexprof-*.tar.gz. Run it after the game has exited.
# env: APPID (3167020), GAME_COMM (Duckov.exe), Q (~/.cache/droiddeck-fexprof), INBOX, OUTBOX, GUEST_TMP (/tmp).
A=${APPID:-3167020}
G=${GAME_COMM:-Duckov.exe}
Q=${Q:-$HOME/.cache/droiddeck-fexprof}
FILES=/storage/emulated/0/Android/data/com.droiddeck.launcher.g2/files
INBOX=${INBOX:-$FILES/agent-inbox}
OUTBOX=${OUTBOX:-$FILES/fexprof}
F=$HOME/.config/droiddeck/game-environment.json
S=$HOME/.local/share/Steam
PFX=$S/steamapps/compatdata/$A/pfx
M=$Q/env-set
GT=${GUEST_TMP:-/tmp}
export GAME_COMM=$G

envjson() { # set MODE | unset
  [ -f "$F" ] || { echo "no $F (start the session first)"; exit 1; }
  python3 - "$F" "$A" "$1" "${2:-block}" <<'PY'
import json, os, sys
f, appid, what, mode = sys.argv[1:]
modes = {
    "block": {"FEX_BLOCKJITNAMING": "1", "FEX_DISKCACHE": "0"},
    "library": {"FEX_LIBRARYJITNAMING": "1", "FEX_DISKCACHE": "0"},
    "none": {},
}
if what == "set" and mode not in modes:
    sys.exit("unknown mode %s (block, library, none)" % mode)
keys = ("FEX_BLOCKJITNAMING", "FEX_LIBRARYJITNAMING", "FEX_GLOBALJITNAMING", "FEX_DISKCACHE", "FEXPROF_MODE")
c = json.load(open(f))
g = c.setdefault("games", {}).setdefault(appid, {})
if what == "set":
    own = [k for k in keys if k in g]
    if own and "FEXPROF_MODE" not in g:
        sys.exit("games[%s] already has its own %s: not touching it" % (appid, ", ".join(own)))
    for k in keys:
        g.pop(k, None)
    g.update(modes[mode])
    g["FEXPROF_MODE"] = mode
elif "FEXPROF_MODE" in g:
    for k in keys:
        g.pop(k, None)
if not g:
    del c["games"][appid]
with open(f + ".fexprof-tmp", "w") as out:
    json.dump(c, out, indent=1)
os.replace(f + ".fexprof-tmp", f)
print("ok games[%s]: %s" % (appid, ", ".join("%s=%s" % (k, g[k]) for k in keys if k in g) or "no profile entries"))
PY
}

case ${1:-} in
  install)
    mkdir -p "$Q" || exit 1
    for f in guestsnap.py fexstats.py prof-report.py; do
      [ -r "$INBOX/$f" ] || { echo "no $INBOX/$f (adb push it there first)"; exit 1; }
      cp "$INBOX/$f" "$Q/$f.new" && mv "$Q/$f.new" "$Q/$f" || exit 1
    done
    sha256sum "$Q"/*.py | sed 's/^/SHA /' ;;
  env)
    case ${2:-} in
      set)
        envjson set "${3:-block}" || exit 1
        mkdir -p "$Q" || exit 1
        [ -e "$M" ] || : > "$M" || exit 1
        d=$PFX/drive_c
        if [ -d "$d" ] && [ ! -d "$d/tmp" ]; then
          mkdir "$d/tmp" && echo "$d/tmp" > "$M.mkdir" && echo "ok created $d/tmp"
        fi ;;
      unset) envjson unset ;;
      *) echo "env set [block|library|none] | env unset"; exit 2 ;;
    esac ;;
  ids) python3 "$Q/guestsnap.py" ids ;;
  snap)
    [ -n "${2:-}" ] || { echo "snap TAG"; exit 2; }
    t=$2; shift 2
    python3 "$Q/guestsnap.py" snap "$Q/$t" "$@" ;;
  stats)
    p=$(python3 "$Q/guestsnap.py" ids | sed -n 's/^PID //p')
    [ -n "$p" ] || { echo "GAME none"; exit 3; }
    python3 "$Q/fexstats.py" rate "$p" --seconds "${2:-10}" ;;
  pack)
    [ -d "$Q/${2:-}" ] && [ -n "${2:-}" ] || { echo "no $Q/${2:-TAG}"; exit 1; }
    mkdir -p "$OUTBOX" || exit 1
    tar -C "$Q" -czf "$OUTBOX/fexprof-$2.tar.gz" "$2" || exit 1
    echo "PACK $OUTBOX/fexprof-$2.tar.gz $(wc -c < "$OUTBOX/fexprof-$2.tar.gz") bytes" ;;
  restore)
    [ -f "$F" ] && envjson unset
    if [ -e "$M" ]; then
      n=0
      for t in "$GT" "$PFX"/dosdevices/?:/tmp "$PFX/drive_c/tmp"; do
        [ -d "$t" ] || continue
        k=$(find "$t/" -maxdepth 1 -type f -name 'perf-*.map' -newer "$M" -print -delete 2>/dev/null | wc -l)
        n=$((n + k))
      done
      echo "ok removed $n perf-*.map"
      if [ -s "$M.mkdir" ]; then d=$(cat "$M.mkdir"); rmdir "$d" 2>/dev/null && echo "ok removed $d"; fi
    fi
    rm -rf "$Q" "$OUTBOX"/fexprof-*.tar.gz
    rmdir "$OUTBOX" 2>/dev/null
    echo "ok removed $Q" ;;
  *) sed -n '/^#!/d;/^#/!q;p' "$0"; exit 2 ;;
esac
