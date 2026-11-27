package com.droiddeck.launcher.session

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
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
 * device or of adbd. The value is put back when the system announces a shutdown or restart; after one it did not
 * announce (a crash of the system, the battery cut off) or with adbd stopped, the next app or session start puts it
 * back. On the O3 the panel went to 144 Hz within about a second; Duckov's frame rate, pacing and temperatures stayed
 * as they were.
 *
 * Shell calls run on one background thread, in order ([RefreshRateLiftDriver]); with wireless debugging off none is
 * tried. A value that could not be put back, or a rate that could not be written while shown, is tried again after
 * [RefreshRateLiftDriver.RETRY_MS] and when wireless debugging comes on, for as long as this process lives.
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
    private const val WIRELESS_DEBUGGING = "adb_wifi_enabled"
    /** Watched for re-pacing when the app may read them; the display's mode changes are followed in any case. */
    val WATCHED = listOf(RefreshRateLiftLogic.KEY, "miui_refresh_rate")

    private val executor = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "refresh-lift").apply { isDaemon = true } }

    private val scheduler = object : RefreshRateLiftDriver.Scheduler {
        override fun execute(task: () -> Unit) {
            executor.execute(Runnable { task() })
        }

        override fun schedule(delayMs: Long, task: () -> Unit): RefreshRateLiftDriver.Cancel {
            val future = executor.schedule(Runnable { task() }, delayMs, TimeUnit.MILLISECONDS)
            return RefreshRateLiftDriver.Cancel { future.cancel(false) }
        }
    }

    @Volatile
    private var driver: RefreshRateLiftDriver? = null
    // Used on the executor's thread only.
    private var shell: Shell? = null
    private var debuggingObserver: ContentObserver? = null

    val available: Boolean get() = Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL == "O3"

    /** For the settings page: whether the setting is on, or null where it is not offered (the panel has one rate only). */
    fun setting(context: Context): Boolean? {
        if (!available || modesHz(context).map { Math.round(it) }.distinct().size < 2) return null
        return SessionPrefs.refreshLift(context)
    }

    fun setEnabled(context: Context, on: Boolean) {
        SessionPrefs.setRefreshLift(context, on)
        if (!on && available) driver(context).turnedOff()
    }

    /** A session activity is on screen: started and not in picture-in-picture. */
    fun shown(context: Context) {
        if (!available) return
        driver(context).shown()
    }

    /**
     * The rate the session asks the panel for, [hz] (0: nothing in particular, the top rate): written as the panel's
     * mode for it once it has been asked for [RefreshRateLiftLogic.STEADY_MS] and the last write is
     * [RefreshRateLiftLogic.DWELL_MS] old, while a session activity is on screen.
     */
    fun want(context: Context, hz: Float) {
        if (!available) return
        driver(context).want(hz)
    }

    /** No session activity is on screen any more; [grace] lets one that is recreated keep the setting raised. */
    fun hidden(context: Context, grace: Boolean = true) {
        if (!available) return
        driver(context).hidden(grace)
    }

    /** At a session's start. The line is for the log. */
    fun sessionStarted(context: Context): String {
        if (!available) return "refresh lift: not on this SoC"
        val on = SessionPrefs.refreshLift(context)
        driver(context).sessionStarted(on)
        return if (on) "refresh lift: ${RefreshRateLiftLogic.KEY} set to the rate the session wants (the panel's top rate unless " +
            "ZeroFG or a frame rate limit asks for another) while on screen (see the $TAG lines)"
        else "refresh lift: off"
    }

    /** At a session's end. */
    fun sessionEnded(context: Context) {
        if (!available) return
        driver(context).sessionEnded()
    }

    /** At app start: a value left raised by a process that died is put back. */
    fun appStarted(context: Context) {
        if (!available || Prefs(context).load() == null) return
        driver(context).appStarted()
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

    private fun driver(context: Context): RefreshRateLiftDriver {
        driver?.let { return it }
        synchronized(this) {
            driver?.let { return it }
            val app = context.applicationContext
            val events = object : RefreshRateLiftDriver.Events {
                override fun line(text: String) {
                    Log.i(TAG, text)
                }

                override fun failed(t: Throwable) {
                    if (t is NoPairing) Log.i(TAG, "${t.message}: ${RefreshRateLiftLogic.KEY} left as it is")
                    else Log.w(TAG, "${RefreshRateLiftLogic.KEY} left as it is", t)
                }

                override fun callDone() {
                    shell?.close()
                }

                override fun watchDebugging(on: Boolean) = watchDebuggingSetting(app, on)
            }
            return RefreshRateLiftDriver(scheduler, { SessionPrefs.refreshLift(app) }, { modesHz(app) }, events) {
                watchShutdown(app)
                RefreshRateLiftLogic(Shell(app).also { shell = it }, Prefs(app), SystemClock::elapsedRealtime) { newId() }
            }.also { driver = it }
        }
    }

    /**
     * The restorer, a child of adbd, ends when the device shuts down or restarts, while the setting stays: the value is
     * put back when the shutdown is announced, within the time the system gives the broadcast.
     */
    private fun watchShutdown(app: Context) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                if (intent.action != Intent.ACTION_SHUTDOWN) return
                val result = goAsync()
                driver(app).shutdown { result.finish() }
            }
        }
        val filter = IntentFilter(Intent.ACTION_SHUTDOWN)
        runCatching {
            if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
            else app.registerReceiver(receiver, filter)
        }.onFailure { Log.w(TAG, "shutdown not watched", it) }
    }

    /** While a retry waits: wireless debugging coming on brings it forward. */
    private fun watchDebuggingSetting(app: Context, on: Boolean) {
        if (on == (debuggingObserver != null)) return
        if (!on) {
            debuggingObserver?.let { observer -> runCatching { app.contentResolver.unregisterContentObserver(observer) } }
            debuggingObserver = null
            return
        }
        val observer = object : ContentObserver(null) {
            override fun onChange(selfChange: Boolean) {
                if (wirelessDebuggingOn(app)) driver(app).debuggingCameOn()
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

    /** The setting through the pairing; connected on first use, closed after each call of the logic. */
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
