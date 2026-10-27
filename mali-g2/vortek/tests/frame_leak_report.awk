# Summarizes frame_leak_test output (with the server's or the wrapper's log, *_CMD_RELEASE_LOG=1):
#   awk -v expect=grow|flat|bounded -v max=64 [-v maxpeak=KB] [-v maxrel=N] -f frame_leak_report.awk <logs...>
# heap: from the "FL frame ... srvheap K" lines; growth per frame from the second report to the last
# ("all"), and of the peak: the highest report of the last half over the highest of the first half
# (from the second report), per frame of the last half ("peak growth"; the heap of a released pool
# saws up and down, its peak stays); steady = mean heap of the last half;
# releases = "command memory: release #" lines (the estimate's), per 1000 frames; CPU from "FL cpu".
#   grow     PASS when the growth from the second report exceeds max bytes/frame (the leak is there)
#   flat     PASS when it does not
#   bounded  PASS when the peak growth does not exceed max, the peak is at most maxpeak kB above the
#            second report (if given) and there are at most maxrel releases per 1000 frames (if given)
/^FL frame .* srvheap / { f[n] = $3; h[n] = $NF; if ($NF > peak) peak = $NF; n++ }
/^FL cpu / { cf[cn] = $3; cms[cn] = $5; cn++ }
/command memory: release #/ { releases++ }
/^FL done:/ { frames = $3 }
END {
    if (n < 4) { print "FAIL too few FL frame reports"; exit }
    mid = int(n / 2)
    all = (h[n-1] - h[1]) * 1024 / (f[n-1] - f[1])
    for (i = 1; i < mid; i++) if (h[i] > peak1) peak1 = h[i]
    for (i = mid; i < n; i++) if (h[i] > peak2) peak2 = h[i]
    half = (peak2 - peak1) * 1024 / (f[n-1] - f[mid-1])
    for (i = mid; i < n; i++) steady += h[i]
    steady /= n - mid
    if (!frames) frames = f[n-1]
    per1000 = releases * 1000 / frames
    cpu = ""
    if (cn >= 4) {
        cm = int(cn / 2)
        cpu = sprintf(", CPU %.1f us/frame over the last half", (cms[cn-1] - cms[cm]) * 1000 / (cf[cn-1] - cf[cm]))
    }
    printf "heap %d kB at frame %d, peak %d kB, steady %d kB; growth %.0f bytes/frame (peak growth %.0f); %d releases in %d frames = %.2f per 1000 frames%s: ",
        h[1], f[1], peak, steady, all, half, releases, frames, per1000, cpu
    if (expect == "grow") { print (all > max ? "PASS (grows without the release)" : "FAIL no growth to fix"); exit }
    if (expect == "flat") { print (all <= max ? "PASS" : "FAIL growth above " max " bytes/frame"); exit }
    if (half > max) { print "FAIL still growing: the peak by " half " bytes/frame over the last half"; exit }
    if (maxpeak != "" && peak - h[1] > maxpeak) { print "FAIL peak " peak - h[1] " kB above the second report (limit " maxpeak ")"; exit }
    if (maxrel != "" && per1000 > maxrel) { print "FAIL " per1000 " releases per 1000 frames (limit " maxrel ")"; exit }
    print "PASS (bounded)"
}
