package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import android.util.Log
import com.droiddeck.launcher.core.GlobalSettings
import com.droiddeck.launcher.core.WirelessAdbFix
import java.util.Locale

/**
 * [mali-g2] persist.sys.miuibooster.thermalbreaklimit on the Xring O3, read at a session's start and logged.
 *
 * MiBridge grants CPU floors only while the board sensor reads below this temperature, in milli-degrees C
 * ([UNSET_MILLI_C] when unset); system_server reads it once, at boot. The owner keeps it at [EXPECTED]. HyperOS 3.1
 * lets a shell setprop of persist.sys.* do nothing without an error, so a value other than [EXPECTED] is logged as a
 * warning. The app only reads it: with its own property read, else with [COMMAND] as the shell user through the
 * wireless debugging pairing.
 */
object ThermalBreakLimit {
    private const val TAG = "ThermalBreakLimit"
    const val PROPERTY = "persist.sys.miuibooster.thermalbreaklimit"
    const val EXPECTED = "50000"
    internal const val UNSET_MILLI_C = 40_000
    internal const val COMMAND = "getprop $PROPERTY"
    private const val WIRELESS_DEBUGGING = "adb_wifi_enabled"

    val available: Boolean get() = Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL == "O3"

    @Volatile
    private var worker: Thread? = null

    internal data class Report(val warning: Boolean, val line: String)

    /** The log line for [value] ("" when unset) read [via] something; null [value]: not read, [via] says why. */
    internal fun report(value: String?, via: String): Report {
        if (value == null) return Report(false, "thermal break limit: $PROPERTY not read ($via)")
        val v = value.trim()
        if (v == EXPECTED) return Report(false, "thermal break limit: $PROPERTY=$v ($via)")
        val shown = if (v.isEmpty()) "is unset" else "=$v"
        val milli = if (v.isEmpty()) UNSET_MILLI_C else v.toIntOrNull()
        val effect = milli?.let { ": MiBridge refuses CPU floors from ${celsius(it)} on the board sensor" } ?: ""
        return Report(true, "thermal break limit: $PROPERTY $shown ($via), not $EXPECTED$effect; system_server reads it at boot only")
    }

    private fun celsius(milli: Int): String =
        if (milli % 1000 == 0) "${milli / 1000} C" else String.format(Locale.US, "%.1f C", milli / 1000.0)

    /** At a session's start: reads and logs the value off the calling thread; [fact] gets the line. */
    @Synchronized
    fun start(context: Context, fact: (String) -> Unit = {}) {
        worker?.interrupt()
        worker = null
        if (!available) {
            val line = "thermal break limit: not on this SoC"
            Log.i(TAG, line)
            fact(line)
            return
        }
        val app = context.applicationContext
        val thread = Thread({
            val outcome = runCatching { read(app) }.getOrElse { report(null, "failed: ${it.message}") }
            // A later session's read replaced this one.
            if (worker !== Thread.currentThread()) return@Thread
            if (outcome.warning) Log.w(TAG, outcome.line) else Log.i(TAG, outcome.line)
            fact(outcome.line)
        }, "thermal-break-limit").apply { isDaemon = true }
        worker = thread
        thread.start()
    }

    private fun read(context: Context): Report {
        // Empty both when unset and when the app may not read it: then the shell user's answer decides.
        ownRead()?.takeIf { it.isNotBlank() }?.let { return report(it, "read by the app") }
        if (!wirelessDebuggingOn(context)) return report(null, "wireless debugging is off")
        val adb = WirelessAdbFix.connectUsingSavedPairing(context)
            ?: return report(null, "no wireless debugging pairing, or wireless debugging is off")
        return adb.use {
            val result = it.shell(COMMAND)
            if (result.exitCode != 0) report(null, "getprop failed (${result.exitCode})")
            else report(result.output, "shell user, through the wireless debugging pairing")
        }
    }

    private fun ownRead(): String? = runCatching {
        Class.forName("android.os.SystemProperties").getMethod("get", String::class.java).invoke(null, PROPERTY) as? String
    }.getOrNull()

    /** Whether wireless debugging is on; true when the app cannot tell. */
    private fun wirelessDebuggingOn(context: Context) =
        runCatching { GlobalSettings.read(context, WIRELESS_DEBUGGING)?.trim() == "1" }.getOrDefault(true)
}
