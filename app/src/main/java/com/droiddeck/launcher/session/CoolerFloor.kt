package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import android.util.Log
import com.droiddeck.launcher.core.WirelessAdbFix

/**
 * [mali-g2] Water cooler mode on the Xring O3: while a game is on screen, a GPU clock floor from the system's perf
 * lock service, and MiBridge asks for GPU and DDR but not CPU ([MIBRIDGE_SPEC]). With a CPU floor as well, the SoC
 * reaches 85 C within seconds and the system clamps every cluster to about 1.3 GHz for 5-8 s, over and over; measured
 * over 15 minutes this point held 77 fps with a low of 75 at half resolution, against 75 with a low of 54.
 *
 * The lock needs the shell user, so it goes through the wireless debugging pairing. It lasts [LOCK_MS] and is renewed
 * every [RENEW_MS], never with duration 0, so it lapses by itself if this process dies. Before each renewal every
 * sensor in [LIMITS] is read: one past its limit, or a required one missing, releases the lock until all are
 * [RESUME_MARGIN_C] back below their limits. The floor can pass the system's thermal cap on the GPU; these limits are
 * what keeps it from doing so unattended.
 */
object CoolerFloor {
    private const val TAG = "CoolerFloor"
    private const val FLOOR_HZ = 1_497_600_000
    private const val RES_GPU_TOP_MIN = 511
    private const val RES_GPU_CORE_MIN = 513
    private const val LOCK_MS = 5_000
    private const val RENEW_MS = 2_500L
    private const val IDLE_MS = 1_000L
    private const val RETRY_MS = 15_000L
    internal const val RESUME_MARGIN_C = 1.0
    const val MIBRIDGE_SPEC = "gpu=1,ddr=1"

    internal val LIMITS = mapOf(
        "quiet_therm" to 42.0, "soc-thermal" to 93.0, "gpu-ss-max" to 90.0, "cpu-ss-max" to 95.0, "battery" to 42.0,
        "charger_therm" to 55.0, "subpmic_therm" to 55.0, "cpu_therm" to 70.0, "flash_therm" to 55.0, "wifi_therm" to 55.0,
    )
    private val REQUIRED = setOf("quiet_therm", "soc-thermal", "gpu-ss-max")
    private val SENSOR = Regex("""mValue=(-?[0-9.]+),.*mName=([^,}]+)""")
    private val REPLY = Regex("""Parcel\(\s*00000000 ([0-9a-f]{8})""")

    /** Set by the play journal: a game's processes are running. */
    @Volatile
    var gameOpen = false

    @Volatile
    private var worker: Thread? = null

    val available: Boolean get() = Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL == "O3"

    /** The HAL's current values from `dumpsys thermalservice`; the cached section before it can be stale. */
    internal fun readings(dump: String): Map<String, Double> {
        val section = dump.substringAfter("Current temperatures from HAL", "").substringBefore("Current cooling devices")
        return SENSOR.findAll(section).associate { it.groupValues[2] to it.groupValues[1].toDouble() }
    }

    /** What stops the floor, or null. [margin] is taken off each limit. */
    internal fun tooHot(temps: Map<String, Double>, margin: Double = 0.0): String? {
        REQUIRED.firstOrNull { it !in temps }?.let { return "$it unreadable" }
        return LIMITS.entries.firstOrNull { (name, limit) -> (temps[name] ?: return@firstOrNull false) >= limit - margin }
            ?.let { "${it.key} ${temps[it.key]}" }
    }

    /** The handle in a `service call perfmanager` reply; 0 when refused. */
    internal fun handleOf(reply: String): Int =
        REPLY.find(reply)?.groupValues?.get(1)?.toLong(16)?.toInt()?.takeIf { it > 0 } ?: 0

    @Synchronized
    fun start(context: Context): String {
        if (!SessionPrefs.coolerMode(context)) return "cooler: off"
        if (!available) return "cooler: not on this SoC"
        val app = context.applicationContext
        worker?.interrupt()
        worker = Thread({ run(app) }, "cooler-floor").apply { isDaemon = true; start() }
        return "cooler: GPU floor ${FLOOR_HZ / 1_000_000} MHz while a game is on screen, MiBridge $MIBRIDGE_SPEC"
    }

    @Synchronized
    fun stop() {
        worker?.interrupt()
        worker = null
        gameOpen = false
    }

    private fun current() = !Thread.currentThread().isInterrupted && worker === Thread.currentThread()

    private fun run(context: Context) {
        try {
            while (current()) {
                val adb = WirelessAdbFix.connectUsingSavedPairing(context)
                if (adb == null) Log.i(TAG, "no wireless debugging pairing, or wireless debugging is off: no floor")
                else try { adb.use { hold(it) } } catch (e: InterruptedException) { throw e } catch (t: Throwable) { Log.w(TAG, "connection lost", t) }
                Thread.sleep(RETRY_MS)
            }
        } catch (_: InterruptedException) {
        }
    }

    private fun hold(adb: com.flyfishxu.kadb.Kadb) {
        val pairs = "i32 4 i32 $RES_GPU_TOP_MIN i32 $FLOOR_HZ i32 $RES_GPU_CORE_MIN i32 $FLOOR_HZ"
        var handle = 0
        var hot = false
        fun release() {
            if (handle != 0) adb.shell("service call perfmanager 3 i32 $handle")
            handle = 0
        }
        try {
            while (current()) {
                if (!gameOpen || !SessionState.visible) {
                    release()
                    Thread.sleep(IDLE_MS)
                    continue
                }
                val temps = readings(adb.shell("dumpsys thermalservice").output)
                val reason = tooHot(temps, if (hot) RESUME_MARGIN_C else 0.0)
                if ((reason != null) != hot) {
                    hot = reason != null
                    Log.i(TAG, if (hot) "released: $reason" else "cool again")
                }
                if (hot) {
                    release()
                    Thread.sleep(RENEW_MS)
                    continue
                }
                val next = handleOf(adb.shell("service call perfmanager 1 i32 0 i32 $LOCK_MS $pairs").output)
                if (next == 0) {
                    Log.w(TAG, "perf lock refused")
                    return
                }
                if (handle == 0) Log.i(TAG, "floor on")
                if (handle != next) release()
                handle = next
                Thread.sleep(RENEW_MS)
            }
        } finally {
            Thread.interrupted()
            runCatching { release() }
        }
    }
}
