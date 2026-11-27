package com.droiddeck.launcher.session

import android.content.Context
import android.database.ContentObserver
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Process
import android.os.SystemClock
import android.provider.Settings
import android.util.Log
import android.view.Display
import com.droiddeck.launcher.core.GlobalSettings
import com.droiddeck.launcher.core.WirelessAdbFix
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/**
 * [mali-g2] The refresh rate the session wants while a game session is on screen on the Xring O3 (on by default there).
 *
 * HyperOS runs the panel at the rate chosen in Settings > Display, Secure user_refresh_rate (120 Hz), whatever mode or
 * frame rate a third-party app asks for: PowerKeeper writes it for the foreground app to Secure miui_refresh_rate, and
 * the compositor follows it. The app may not write Secure settings, the shell user may: so it is set through the
 * wireless debugging pairing while a session activity is started and not in picture-in-picture, and put back afterwards
 * ([RefreshRateLiftLogic] has the rules). The rate is the one the session asks for ([want]: ZeroFG's frame-rate vote,
 * the mode a frame rate limit divides), else the panel's top rate (144 Hz), as a mode of the panel's; a change is
 * written once it has held for a while, at most every few seconds. A restorer started as the shell user puts the value
 * back once this process is gone (crash, force-stop, an update, the app's data cleared); it ends with a restart of the
 * device or of adbd, and then the next app or session start puts the value back. On the O3 the panel went to 144 Hz
 * within about a second; Duckov's frame rate, pacing and temperatures stayed as they were.
 *
 * Shell calls run on one background thread, in order; with wireless debugging off none is tried. A value that could not
 * be put back is tried again after [RETRY_MS] and when wireless debugging comes on, for as long as this process lives.
 */
object RefreshRateLift {
    private const val TAG = "RefreshRateLift"
    /** Session prefs: the value written; set while this app has the setting raised. */
    internal const val WRITTEN = "refreshLiftWritten"
    /** Session prefs: the value before, absent when it was unset. */
    internal const val ORIGINAL = "refreshLiftOriginal"
    /** Session prefs: the restorer watching the value written. */
    internal const val RESTORER = "refreshLiftRestorer"
    /** Session prefs: false until the write went through. */
    internal const val CONFIRMED = "refreshLiftConfirmed"
    /** Session prefs: the value written before, while the write of the one after has not gone through. */
    internal const val PREVIOUS = "refreshLiftPrevious"
    /** Session activities stopping and starting again (recreated, a dialog over them) keep the setting raised. */
    private const val HIDE_GRACE_MS = 1_500L
    /** Waits before trying again to put the value back, one per failure in a row; the last repeats. */
    private val RETRY_MS = longArrayOf(15_000L, 30_000L, 60_000L, 120_000L, 300_000L, 600_000L, 1_200_000L, 2_400_000L, 3_600_000L)
    /** After wireless debugging comes on, for adbd to listen. */
    private const val WIRELESS_DEBUGGING_ON_MS = 3_000L
    private const val WIRELESS_DEBUGGING = "adb_wifi_enabled"
    /** Watched for re-pacing when the app may read them; the display's mode changes are followed in any case. */
    val WATCHED = listOf(RefreshRateLiftLogic.KEY, "miui_refresh_rate")

    private val executor = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "refresh-lift").apply { isDaemon = true } }
    private var pendingHide: ScheduledFuture<*>? = null
    // Used on the executor's thread only.
    private var shell: Shell? = null
    private var logic: RefreshRateLiftLogic? = null
    private var failureLogged = false
    private var retries = 0
    private var retry: ScheduledFuture<*>? = null
    private var change: ScheduledFuture<*>? = null
    private var debuggingObserver: ContentObserver? = null

    /** The rate the session asks for ([want]); 0: nothing in particular. */
    @Volatile
    private var wantedHz = 0f

    val available: Boolean get() = Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL == "O3"

    /** For the settings page: whether the setting is on, or null where it is not offered (the panel has one rate only). */
    fun setting(context: Context): Boolean? {
        if (!available || modesHz(context).map { Math.round(it) }.distinct().size < 2) return null
        return SessionPrefs.refreshLift(context)
    }

    fun setEnabled(context: Context, on: Boolean) {
        SessionPrefs.setRefreshLift(context, on)
        if (!on && available) submit(context) { _, logic -> logic.turnedOff() }
    }

    /** A session activity is on screen: started and not in picture-in-picture. */
    @Synchronized
    fun shown(context: Context) {
        if (!available) return
        pendingHide?.cancel(false)
        pendingHide = null
        submit(context) { app, logic -> modes(app).let { (top, hz) -> logic.shown(SessionPrefs.refreshLift(app), top, hz) } }
    }

    /**
     * The rate the session asks the panel for, [hz] (0: nothing in particular, the top rate): written as the panel's
     * mode for it once it has been asked for [RefreshRateLiftLogic.STEADY_MS] and the last write is
     * [RefreshRateLiftLogic.DWELL_MS] old, while a session activity is on screen.
     */
    fun want(context: Context, hz: Float) {
        if (!available) return
        val next = if (hz > 1f) hz else 0f
        if (kotlin.math.abs(next - wantedHz) < 0.01f) return
        wantedHz = next
        val app = context.applicationContext
        executor.execute {
            val logic = logic(app)
            modes(app).let { (top, rate) -> logic.want(rate, top) }
            plan(app, logic)
        }
    }

    /** No session activity is on screen any more; [grace] lets one that is recreated keep the setting raised. */
    @Synchronized
    fun hidden(context: Context, grace: Boolean = true) {
        if (!available) return
        val app = context.applicationContext
        pendingHide?.cancel(false)
        pendingHide = null
        if (!grace) {
            submit(app) { _, logic -> logic.hidden() }
            return
        }
        pendingHide = executor.schedule({ run(app) { _, logic -> logic.hidden() } }, HIDE_GRACE_MS, TimeUnit.MILLISECONDS)
    }

    /** At a session's start. The line is for the log. */
    fun sessionStarted(context: Context): String {
        if (!available) return "refresh lift: not on this SoC"
        val on = SessionPrefs.refreshLift(context)
        submit(context) { app, logic -> modes(app).let { (top, hz) -> logic.sessionStarted(on, top, hz) } }
        return if (on) "refresh lift: ${RefreshRateLiftLogic.KEY} set to the rate the session wants (the panel's top rate unless " +
            "ZeroFG or a frame rate limit asks for another) while on screen (see the $TAG lines)"
        else "refresh lift: off"
    }

    /** At a session's end. */
    fun sessionEnded(context: Context) {
        if (!available) return
        submit(context) { _, logic -> logic.sessionEnded() }
        executor.execute { failureLogged = false }
    }

    /** At app start: a value left raised by a process that died is put back. */
    fun appStarted(context: Context) {
        if (!available || Prefs(context).load() == null) return
        submit(context) { _, logic -> logic.appStarted() }
    }

    /** Has [observer] told of changes to the [WATCHED] settings this app may read. */
    fun watch(context: Context, observer: ContentObserver) {
        for (name in WATCHED) {
            if (runCatching { Settings.Secure.getString(context.contentResolver, name) }.isFailure) continue
            context.contentResolver.registerContentObserver(Settings.Secure.getUriFor(name), false, observer)
        }
    }

    /** The default display's refresh rates at its current size; empty when unknown. */
    private fun modesHz(context: Context): List<Float> {
        val display = context.getSystemService(DisplayManager::class.java)?.getDisplay(Display.DEFAULT_DISPLAY) ?: return emptyList()
        val mode = display.mode
        return display.supportedModes
            .filter { it.physicalWidth == mode.physicalWidth && it.physicalHeight == mode.physicalHeight }
            .map { it.refreshRate }
    }

    /** The default display's top rate and its mode for the rate wanted, 0 when unknown. */
    private fun modes(context: Context): Pair<Int, Int> {
        val modes = runCatching { modesHz(context) }.getOrDefault(emptyList())
        return RefreshRateLiftLogic.modeFor(0f, modes) to RefreshRateLiftLogic.modeFor(wantedHz, modes)
    }

    private fun logic(app: Context): RefreshRateLiftLogic {
        val shell = shell ?: Shell(app).also { shell = it }
        return logic ?: RefreshRateLiftLogic(shell, Prefs(app), SystemClock::elapsedRealtime) { newId() }.also { logic = it }
    }

    private fun submit(context: Context, block: (Context, RefreshRateLiftLogic) -> String?) {
        val app = context.applicationContext
        executor.execute { run(app, block) }
    }

    private fun run(app: Context, block: (Context, RefreshRateLiftLogic) -> String?) {
        val logic = logic(app)
        val shell = shell!!
        var failure: Throwable? = null
        try {
            block(app, logic)?.let { Log.i(TAG, it) }
        } catch (t: Throwable) {
            failure = t
            if (!failureLogged) {
                failureLogged = true
                if (t is NoPairing) Log.i(TAG, "${t.message}: ${RefreshRateLiftLogic.KEY} left as it is")
                else Log.w(TAG, "${RefreshRateLiftLogic.KEY} left as it is", t)
            }
        } finally {
            shell.close()
        }
        settleRetry(app, logic, failure)
        plan(app, logic)
    }

    /** Has the rate wanted written once it is due ([RefreshRateLiftLogic.due]). */
    private fun plan(app: Context, logic: RefreshRateLiftLogic) {
        change?.cancel(false)
        change = null
        val delay = runCatching { logic.due(SessionPrefs.refreshLift(app)) }.getOrNull() ?: return
        change = executor.schedule(Runnable { run(app) { a, l -> l.settle(SessionPrefs.refreshLift(a)) } }, delay, TimeUnit.MILLISECONDS)
    }

    /** After a call: tries again later when it failed and a raised value waits to be put back, else stops trying. */
    private fun settleRetry(app: Context, logic: RefreshRateLiftLogic, failure: Throwable?) {
        retry?.cancel(false)
        retry = null
        val waiting = failure != null && runCatching { logic.pending(SessionPrefs.refreshLift(app)) }.getOrDefault(false)
        if (!waiting) {
            retries = 0
            watchDebugging(app, false)
            return
        }
        val delay = RETRY_MS[retries.coerceAtMost(RETRY_MS.size - 1)]
        if (retries == 0) {
            Log.i(TAG, "${RefreshRateLiftLogic.KEY} not put back (${failure?.message}): trying again in ${delay / 1000} s, " +
                "then less often, and when wireless debugging comes on")
        }
        retries++
        retry = executor.schedule(Runnable { retryNow(app) }, delay, TimeUnit.MILLISECONDS)
        watchDebugging(app, true)
    }

    private fun retryNow(app: Context) {
        run(app) { a, logic -> logic.retry(SessionPrefs.refreshLift(a)) }
    }

    /** While a retry waits: wireless debugging coming on brings it forward. */
    private fun watchDebugging(app: Context, on: Boolean) {
        if (on == (debuggingObserver != null)) return
        if (!on) {
            debuggingObserver?.let { observer -> runCatching { app.contentResolver.unregisterContentObserver(observer) } }
            debuggingObserver = null
            return
        }
        val observer = object : ContentObserver(null) {
            override fun onChange(selfChange: Boolean) {
                if (!wirelessDebuggingOn(app)) return
                executor.schedule(Runnable {
                    val waiting = retry ?: return@Runnable
                    waiting.cancel(false)
                    retry = null
                    retryNow(app)
                }, WIRELESS_DEBUGGING_ON_MS, TimeUnit.MILLISECONDS)
            }
        }
        if (runCatching { app.contentResolver.registerContentObserver(Settings.Global.getUriFor(WIRELESS_DEBUGGING), false, observer) }.isSuccess) {
            debuggingObserver = observer
        }
    }

    /** Whether wireless debugging is on; true when the app cannot tell. */
    private fun wirelessDebuggingOn(context: Context) =
        runCatching { GlobalSettings.read(context, WIRELESS_DEBUGGING)?.trim() == "1" }.getOrDefault(true)

    private fun newId() = UUID.randomUUID().toString().replace("-", "").take(16)

    private class NoPairing : Exception("no wireless debugging pairing, or wireless debugging is off")

    /** The setting through the pairing; connected on first use, closed after each call of [run]. */
    private class Shell(private val context: Context) : RefreshRateLiftLogic.Secure {
        private val markerFile = RefreshRateLiftLogic.markerPath(context.packageName)
        private var adb: com.flyfishxu.kadb.Kadb? = null

        private fun command(line: String): String {
            val adb = adb ?: connect().also { adb = it }
            val result = adb.shell(line)
            check(result.exitCode == 0) { "'${line.take(80)}' failed (${result.exitCode}): ${result.allOutput.trim().take(200)}" }
            return result.output
        }

        /** With wireless debugging off, the saved pairing's port lookup (up to about 20 s, holding other callers back) is not tried. */
        private fun connect(): com.flyfishxu.kadb.Kadb {
            if (!wirelessDebuggingOn(context)) throw NoPairing()
            return WirelessAdbFix.connectUsingSavedPairing(context) ?: throw NoPairing()
        }

        override fun read() = RefreshRateLiftLogic.parse(command("settings get secure ${RefreshRateLiftLogic.KEY}"))

        override fun put(value: String) {
            require(RefreshRateLiftLogic.isRate(value)) { "not a rate: '$value'" }
            command("settings put secure ${RefreshRateLiftLogic.KEY} $value")
        }

        override fun putBack(original: String?) {
            command(RefreshRateLiftLogic.putBackCommand(markerFile, original))
        }

        override fun marker() = RefreshRateLiftLogic.parseMarker(command("cat $markerFile 2>/dev/null; true"))

        override fun arm(record: RefreshRateLiftLogic.Record) {
            command(RefreshRateLiftLogic.armCommand(markerFile, Process.myPid(), record))
        }

        override fun change(record: RefreshRateLiftLogic.Record) {
            command(RefreshRateLiftLogic.changeCommand(markerFile, record))
        }

        override fun disarm() {
            command("rm -f $markerFile")
        }

        fun close() {
            adb?.let { runCatching { it.close() } }
            adb = null
        }
    }

    private class Prefs(context: Context) : RefreshRateLiftLogic.Records {
        private val prefs = context.applicationContext.getSharedPreferences("session", Context.MODE_PRIVATE)

        // A record without a restorer comes from a build before the restorer; its write had gone through.
        override fun load() = prefs.getString(WRITTEN, null)?.let {
            RefreshRateLiftLogic.Record(prefs.getString(ORIGINAL, null), it, prefs.getString(RESTORER, null) ?: "0", prefs.getBoolean(CONFIRMED, true),
                prefs.getString(PREVIOUS, null))
        }

        override fun save(record: RefreshRateLiftLogic.Record) {
            val edit = prefs.edit().putString(WRITTEN, record.written).putString(RESTORER, record.id).putBoolean(CONFIRMED, record.confirmed)
            if (record.original == null) edit.remove(ORIGINAL) else edit.putString(ORIGINAL, record.original)
            if (record.previous == null) edit.remove(PREVIOUS) else edit.putString(PREVIOUS, record.previous)
            check(edit.commit()) { "could not record ${RefreshRateLiftLogic.KEY}" }
        }

        override fun clear() {
            prefs.edit().remove(WRITTEN).remove(ORIGINAL).remove(RESTORER).remove(CONFIRMED).remove(PREVIOUS).commit()
        }
    }
}
