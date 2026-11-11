package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import android.os.PerformanceHintManager
import android.os.PowerManager
import android.os.Process
import android.system.Os
import android.system.OsConstants
import android.util.Log
import com.droiddeck.launcher.wayland.WaylandCompositor
import java.io.File
import kotlin.math.abs

/**
 * The game's busiest threads named to Android's performance hint service (ADPF) for a session.
 *
 * Qualcomm's WALT moves a thread to the prime cores only once it fills a mid core (the Odin 3's
 * sched_upmigrate is 95), and a game thread that waits on its workers or the GPU for part of each
 * frame never does: Factorio's update thread sat at ~68% on cores 0-5 with 6-7 idle, and ran 60%
 * faster pinned to 6-7. An app may not raise a thread's uclamp (EPERM without CAP_SYS_NICE), but a
 * hint session may name any thread of its UID - the guest's are - and one reported behind its
 * target is boosted by the power HAL onto the prime cores: on the Odin 3 a 70%-busy thread of a
 * child process went from cores 0-5 to 6-7 and ran 1.7x faster within a second.
 *
 * Once a second the busiest threads of this UID are read from /proc; one busy more than [ENTER] of
 * the time (averaged with the second before, so a loading spike does not count) joins the session,
 * and leaves it under [LEAVE] - well below, since it runs faster where it was moved. While the
 * session has threads it reports them behind target, which is what asks for the boost: the reports
 * carry no frame timing, the busy share is the signal (a capped game's frame interval looks the same
 * as a slow one's). Nothing is boosted while the device is critically hot or saves power.
 *
 * Nor while the game holds the panel's rate or the session's frame rate limit ([holdsPace]): there is nothing to
 * gain, and it cost frame pacing - Serious Sam capped at 120 fps ran its boosted main thread on prime
 * cores the governor kept at 2.2 GHz, slower than a 3.5 GHz mid core, and 23-25% of its frames fell
 * under 90 fps against 2-7% unboosted. A game with no frames (Factorio's benchmark, a loading screen)
 * or one falling behind the panel is boosted; one holding pace keeps what it has for [PACE_LEAVE_S]
 * seconds and is then let go. Half or a third of the panel's rate is no limiter: nothing in the session
 * halves the rate of a game that misses a refresh (no gamescope, the layers are FIFO at the panel's
 * rate), so a CPU-bound game near 60 fps on a 120 Hz panel is just slow. Counting it as paced trapped
 * Duckov: a dip to ~60 fps (a loading screen) let its main thread go from a 4.2 GHz prime core to a
 * mid core, where it stayed at 58-60 fps instead of 73.
 *
 * [mali-g2] On the Xring O3 the hint sessions do nothing: its power HAL is AOSP's example service (see
 * XrThreadQos), so none are opened there while it answers the example's 1 ms update rate ([stubPowerHal]);
 * firmware with a HAL of its own gets them back. The scan still runs for ClusterSteer and FrameSchedFeed,
 * and the threads it picks may be given one of HyperOS's own thread QoS levels (pref xringThreadQos, off
 * until measured), taken away again when they leave the set.
 */
object GameThreadHints {
    private val HINTED_SOCS = setOf("CQ8725S", "O3")
    private const val TAG = "GameThreadHints"
    private const val TARGET_NS = 10_000_000L
    private const val REPORT_MS = 50L
    private const val SCAN_EVERY = 20
    private const val ENTER = 0.6
    private const val LEAVE = 0.15
    private const val MAX_THREADS = 2
    private const val TOP_PROCESSES = 3
    private const val PACE_WINDOW_S = 3
    private const val PACE_LEAVE_S = 10
    private const val PACE_TOLERANCE = 0.08
    private const val QOS_VERIFY_SCANS = 10 // the QoS levels given are read back every ten seconds
    private const val PREVIOUS_WORKER_MS = 7_000L // ClusterSteer's taskset waits up to 5 s
    /** The update rate AOSP's example power HAL reports; one that acts on hints reports its own. */
    private const val EXAMPLE_HAL_RATE_NS = 1_000_000L

    private val NOT_GAMES = setOf("steam", "steamwebhelper")

    private var worker: Thread? = null
    /** The worker stop() interrupted, which may still be undoing its QoS levels and affinities. */
    private var retired: Thread? = null

    @Synchronized
    fun start(context: Context): String {
        if (!SessionPrefs.gameThreadHints(context)) return "game thread hints: off"
        // The hint sessions were measured boosting on the Odin 3 (CQ8725S); on Xiaomi's Xring O3 they reach
        // nothing (see XrThreadQos), but the scan drives ClusterSteer, XrThreadQos and FrameSchedFeed there.
        // Another HAL may boost differently, or not at all.
        if (Build.VERSION.SDK_INT < 31 || Build.SOC_MODEL !in HINTED_SOCS) return "game thread hints: not on this SoC"
        val hm = context.getSystemService(PerformanceHintManager::class.java)
            ?: return "game thread hints: no PerformanceHintManager"
        val pm = context.getSystemService(PowerManager::class.java)
        val rate = runCatching { hm.preferredUpdateRateNanos }.getOrDefault(-1L)
        // [mali-g2] A session on the example HAL costs twenty binder calls a second for nothing, and A/B runs
        // credited it with what the scan's other users (cluster steering, xr qos) did.
        val hints = !stubPowerHal(Build.SOC_MODEL, rate)
        val qos = try {
            XrThreadQos.forSession(context)
        } catch (t: Throwable) {
            Log.w(TAG, "xr qos unavailable", t)
            null
        }
        // [mali-g2] Only where a frame regulator was seen holding a cluster's clock (see ClusterSteer), and not
        // with xr qos: moving whole processes to the little cores would take the boosted threads off the cores
        // their level puts them on, and the cluster experiments need the affinities left as set by hand.
        val steerWanted = Build.SOC_MODEL == "O3" && SessionPrefs.clusterSteer(context)
        val steer = steerWanted && qos == null
        val previous = worker ?: retired
        previous?.interrupt()
        retired = null
        worker = Thread({ runAfter(previous, hm.takeIf { hints }, pm, qos, steer) }, "game-thread-hints").apply { isDaemon = true; start() }
        return "game thread hints: watching (hint rate $rate ns${if (hints) "" else ", example power HAL: no hint sessions"}, " +
            "xr qos ${qos ?: "off"}" +
            (if (Build.SOC_MODEL == "O3") ", cluster steering " + when {
                steer -> "on"
                steerWanted -> "off with xr qos"
                else -> "off"
            } else "") + ")"
    }

    /** The last session's worker undoes its QoS levels and affinities first, so they do not land on this one's. */
    private fun runAfter(previous: Thread?, hm: PerformanceHintManager?, pm: PowerManager, qos: XrThreadQos?, steer: Boolean) {
        try {
            previous?.join(PREVIOUS_WORKER_MS)
        } catch (_: InterruptedException) {
            return
        }
        run(hm, pm, qos, steer)
    }

    @Synchronized
    fun stop() {
        worker?.interrupt()
        worker?.let { retired = it }
        worker = null
    }

    /** [hm] null: the threads are picked as usual, for xr qos and the log, but no hint session is opened. */
    private fun run(hm: PerformanceHintManager?, pm: PowerManager, qos: XrThreadQos?, steerOn: Boolean) {
        val scanner = Scanner()
        val steer = if (steerOn) ClusterSteer.forDevice() else null
        var session: PerformanceHintManager.Session? = null
        var boosted = emptySet<Int>()
        var qosOn = emptySet<Int>()
        val frames = ArrayDeque<Long>()
        var pacedFor = 0
        var tick = 0
        try {
            while (!Thread.currentThread().isInterrupted) {
                if (tick++ % SCAN_EVERY == 0) {
                    val busy = scanner.sample()
                    frames.addLast(WaylandCompositor.gameFrameCount())
                    if (frames.size > PACE_WINDOW_S + 1) frames.removeFirst()
                    val fps = (frames.last() - frames.first()).toDouble() / (frames.size - 1).coerceAtLeast(1)
                    val paced = frames.size > PACE_WINDOW_S &&
                        holdsPace(fps, SessionState.refreshHz.toDouble(), SessionState.fpsLimit)
                    pacedFor = if (paced) pacedFor + 1 else 0
                    val busiest = busy.maxByOrNull { it.value }?.let { (tid, share) -> scanner.owner[tid]?.let { Triple(it, tid, share) } }
                    steer?.tick(scanner.top, busiest)?.let { Log.i(TAG, "cluster steering: $it (game ${fps.toInt()} fps)") }
                    busiest?.let { (pid, tid, _) -> FrameSchedFeed.gameThread(pid, tid) }
                    // Not SEVERE: the Odin 3 reports it once its skin reaches 50 C, a couple of minutes into any game.
                    val spare = pm.isPowerSaveMode || pm.currentThermalStatus >= PowerManager.THERMAL_STATUS_CRITICAL
                    val next = when {
                        spare || pacedFor >= PACE_LEAVE_S -> emptySet()
                        paced -> pick(boosted, busy.filterKeys { it in boosted })
                        else -> pick(boosted, busy)
                    }
                    if (next != boosted) {
                        if (hm != null) {
                            session = update(hm, session, next)
                            // No session means a thread exited in between: tried again next second.
                            boosted = if (session == null) emptySet() else next
                        } else boosted = next
                        Log.i(TAG, (if (hm != null && session == null && next.isNotEmpty()) "no session for "
                            else if (hm == null && qos == null) "busiest game threads " else "boosting ") +
                            next.joinToString { "${readName("/proc/$it/comm")}/$it ${((busy[it] ?: 0.0) * 100).toInt()}%" }.ifEmpty { "nothing" } +
                            " (game ${fps.toInt()} fps${if (paced) ", holding pace" else ""})")
                    }
                    // Apart from the hint session, which a thread exiting in between fails as a whole.
                    if (qos != null && next != qosOn) qosOn = retarget(qos, qosOn, next)
                    if (qos != null && tick % (SCAN_EVERY * QOS_VERIFY_SCANS) == 1) {
                        runCatching { qos.verify(qosOn) }.onSuccess { lines -> lines.forEach { Log.i(TAG, it) } }
                            .onFailure { Log.w(TAG, "xr qos read-back", it) }
                    }
                }
                session?.reportActualWorkDuration(TARGET_NS * 3 / 2)
                Thread.sleep(REPORT_MS)
            }
        } catch (_: InterruptedException) {
        } catch (t: Throwable) {
            Log.w(TAG, "stopped", t)
        } finally {
            runCatching { session?.close() }
            if (qos != null) retarget(qos, qosOn, emptySet())
            runCatching { steer?.release() }.onFailure { Log.w(TAG, "cluster steering release", it) }
        }
    }

    /**
     * The QoS level taken from the threads of [on] not in [next] and given to those new in it (once, taken or not); returns [next].
     * Never throws: it runs in the worker's finally, and a missing JNI symbol must not take the app down.
     */
    private fun retarget(qos: XrThreadQos, on: Set<Int>, next: Set<Int>): Set<Int> {
        (on - next).forEach { tid ->
            runCatching { qos.clear(tid) }.onSuccess { it?.let { line -> Log.i(TAG, line) } }.onFailure { Log.w(TAG, "xr qos clear $tid", it) }
        }
        (next - on).forEach { tid ->
            runCatching { qos.apply(tid) }.onSuccess { Log.i(TAG, "${readName("/proc/$tid/comm")}: $it") }
                .onFailure { Log.w(TAG, "xr qos $tid", it) }
        }
        return next
    }

    /** A new session over [tids] in place of [session]; null for none. The set changes seldom, so no setThreads. */
    private fun update(hm: PerformanceHintManager, session: PerformanceHintManager.Session?, tids: Set<Int>): PerformanceHintManager.Session? {
        session?.close()
        if (tids.isEmpty()) return null
        return try {
            hm.createHintSession(tids.toIntArray(), TARGET_NS)?.also {
                // API 35; the app compiles against an older SDK.
                if (Build.VERSION.SDK_INT >= 35) runCatching {
                    it.javaClass.getMethod("setPreferPowerEfficiency", Boolean::class.javaPrimitiveType).invoke(it, false)
                }
            }
        } catch (t: RuntimeException) {
            Log.w(TAG, "createHintSession", t)
            null
        }
    }

    /**
     * [mali-g2] Whether hint sessions would reach AOSP's example power HAL, which acts on none: the Xring O3
     * ([soc]) answering the example's 1 ms [preferredRateNs]. Only there - elsewhere 1 ms says nothing of the HAL.
     */
    internal fun stubPowerHal(soc: String, preferredRateNs: Long): Boolean =
        soc == "O3" && preferredRateNs == EXAMPLE_HAL_RATE_NS

    /**
     * Whether a game presenting [fps] frames a second keeps up: within [PACE_TOLERANCE] of the
     * [refreshHz] panel's rate, or of the session's frame rate limit [fpsLimit] (0: none).
     * A game with next to no frames (a headless benchmark, a loading screen) does not.
     */
    internal fun holdsPace(fps: Double, refreshHz: Double, fpsLimit: Int = 0): Boolean =
        fps >= 10 && listOf(refreshHz, fpsLimit.toDouble()).any { rate -> rate > 0 && abs(fps - rate) < PACE_TOLERANCE * rate }

    internal fun pick(boosted: Set<Int>, busy: Map<Int, Double>): Set<Int> =
        busy.entries
            .filter { (tid, share) -> share >= if (tid in boosted) LEAVE else ENTER }
            .sortedWith(compareByDescending<Map.Entry<Int, Double>> { it.key in boosted }.thenByDescending { it.value })
            .take(MAX_THREADS)
            .map { it.key }
            .toSet()

    private fun readName(path: String) = try { File(path).readText().trim() } catch (_: Exception) { "?" }

    private class Scanner {
        private val self = Process.myPid()
        private val hz = Os.sysconf(OsConstants._SC_CLK_TCK).toDouble()
        private var lastNs = 0L
        private var lastProcess = HashMap<Int, Long>()
        private var lastThread = HashMap<Int, Long>()
        private var lastBusy = HashMap<Int, Double>()

        /** The busiest processes of the last sample, and the process of each thread read. */
        var top: List<Int> = emptyList()
            private set
        val owner = HashMap<Int, Int>()

        /** Only the busiest few processes have their threads read, so a second costs a few dozen small reads. */
        fun sample(): Map<Int, Double> {
            val now = System.nanoTime()
            val seconds = (now - lastNs) / 1e9
            lastNs = now
            val process = HashMap<Int, Long>()
            File("/proc").list()?.forEach { entry ->
                val pid = entry.toIntOrNull() ?: return@forEach
                cpuTicks("/proc/$pid/stat")?.let { process[pid] = it }
            }
            val top = process.entries
                .map { (pid, ticks) -> pid to ticks - (lastProcess[pid] ?: ticks) }
                .filter { (pid, delta) -> delta > 0 && pid != self }
                .sortedByDescending { it.second }
                .asSequence()
                .filter { (pid, _) -> readName("/proc/$pid/comm") !in NOT_GAMES }
                .take(TOP_PROCESSES)
                .toList()
            lastProcess = process
            this.top = top.map { it.first }
            owner.clear()
            val thread = HashMap<Int, Long>()
            val busy = HashMap<Int, Double>()
            for ((pid, _) in top) {
                File("/proc/$pid/task").list()?.forEach { entry ->
                    val tid = entry.toIntOrNull() ?: return@forEach
                    val ticks = cpuTicks("/proc/$pid/task/$tid/stat") ?: return@forEach
                    thread[tid] = ticks
                    owner[tid] = pid
                    lastThread[tid]?.let { before -> busy[tid] = ((ticks - before) / hz / seconds + (lastBusy[tid] ?: 0.0)) / 2 }
                }
            }
            lastThread = thread
            lastBusy = busy
            return busy
        }

        private fun cpuTicks(path: String): Long? = try {
            val line = File(path).readText()
            val fields = line.substring(line.lastIndexOf(')') + 2).split(' ')
            fields[11].toLong() + fields[12].toLong()
        } catch (_: Exception) {
            null
        }
    }
}
