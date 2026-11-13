package com.droiddeck.launcher.session.play

/**
 * [mali-g2] Why a play ended, from what was seen during and after it. The first rule that matches wins;
 * the flags are independent of it.
 *
 * Steam's exit code is only a hint: the wrapper exits 0 when a Unity game crashes (its Player.log has the
 * "Crash!!!"), so a crash is decided by the logs as much as by the code. Only the game's own logs decide
 * it (LogSliceScanner.Source.game): Steam's client and its web helper write to the session's logs too.
 */
object ExitClassifier {
    class AppExit(val reason: String, val status: Int, val description: String?, val atMs: Long)

    class Evidence(
        val durS: Long,
        val appKilled: AppExit? = null,
        val appGone: Boolean = false,
        /** The session was stopped (by the owner, or the app going away) while the game ran. */
        val sessionStopped: Boolean = false,
        val sessionStatus: Int? = null,
        val failureCode: String? = null,
        /** vortek-server died on its own (not stopped by the session) while the play was open. */
        val vortekExit: Int? = null,
        /** Seconds from the server's death to the play's end; null when unknown. */
        val vortekToEndS: Long? = null,
        val usedVortek: Boolean = false,
        val findings: LogSliceScanner.Findings? = null,
        val steamExitCode: Int? = null,
        /** Frames shown in counted time (game layer or composited). */
        val gameFrames: Long? = null,
        val screenFramesSeen: Boolean = false,
        val longestZeroRunS: Long = 0,
        /** The same, for runs that started after the game's first two minutes (loading). */
        val zeroRunAfterLoadS: Long = longestZeroRunS,
        val longestStallS: Double? = null,
        val meanFpsAfterWarmup: Double? = null,
        val low1: Double? = null,
        val at60: Double? = null,
        val at60Thermal: Double? = null,
        val marked: Boolean = false,
        val runMode: Boolean = false,
        /** The session's fps cap; a game held to 20 fps on purpose is not slow. */
        val fpsLimit: Int? = null,
        /**
         * BL_SYNC_SEMPOOL from the game's environment: on (the pack's pool), preload (libblsession.so's pool), off (a pack with the
         * pool fits the Proton build but its pool is not in use) or none (no pack with the pool fits and the preload pool does
         * not run); null when unknown.
         */
        val semPool: String? = null,
    )

    class Exit(
        val reason: String,
        val steamCode: Int? = null,
        val signal: Int? = null,
        val vortek: Int? = null,
        val appExit: AppExit? = null,
        val detail: String? = null,
        val flags: List<String> = emptyList(),
    )

    fun classify(e: Evidence): Exit {
        val f = e.findings
        val crashLines = f?.errors?.get("crash")?.first.orEmpty().filter { line -> f?.gameLabels?.any { line.startsWith("$it: ") } == true }
        val crashInGame = f?.inGame("crash") == true
        val code = e.steamExitCode ?: e.sessionStatus.takeIf { e.runMode && !e.sessionStopped }
        val signal = code?.takeIf { it in 129..192 }?.let { it - 128 }
        val vortekSignal = e.vortekExit?.takeIf { it in 129..192 }?.let { it - 128 }
        val noFrame = (e.gameFrames ?: 0L) == 0L && !e.screenFramesSeen
        val unsupported = f?.inGame("unsupported") == true
        val crashed = crashInGame || (code != null && code != 0 && !(e.runMode && code < 0))
        val reason: String
        var detail: String? = null
        when {
            e.appKilled != null || e.appGone -> {
                reason = "app_killed"
                detail = e.appKilled?.let { a -> "app ${a.reason}" + (a.description?.takeIf { it.isNotBlank() }?.let { ": $it" } ?: "") }
                    ?: "app process gone; no exit record"
            }
            e.sessionStopped -> {
                reason = "session_stopped"
                detail = listOfNotNull(e.sessionStatus?.let { "status $it" }, e.failureCode).joinToString(", ").ifEmpty { null }
            }
            e.vortekExit != null && (e.usedVortek || (e.vortekToEndS != null && e.vortekToEndS <= 20)) -> {
                reason = "vortek_exit"
                detail = "vortek-server exited: ${e.vortekExit}" + (vortekSignal?.let { " (signal $it)" } ?: "")
            }
            f != null && f.inGame("device_lost") -> {
                reason = "device_lost"
                detail = f.firstGameLine("device_lost")
            }
            // Missing features with no frame, or a crash soon after them: the cause, even when the game then crashed on it.
            unsupported && (noFrame || (crashed && e.durS < 120)) -> {
                reason = "launch_failed"
                detail = missingFeature(f)
            }
            crashed -> {
                reason = "crash"
                // The cause rather than the symptom: Unity's "Crash!!!" comes last.
                detail = f?.let { missingFeature(it) ?: it.firstGameLine("oom") ?: it.firstGameLine("vulkan") }
                    ?: crashLines.firstOrNull { !it.contains("Crash!!!") } ?: crashLines.firstOrNull()
                    ?: "exit code $code" + (signal?.let { " (signal $it)" } ?: "")
            }
            e.durS < 60 && noFrame -> {
                reason = "launch_failed"
                detail = "ended within ${e.durS} s without a game frame"
            }
            else -> reason = "normal"
        }
        val flags = ArrayList<String>()
        if (reason in setOf("crash", "device_lost", "vortek_exit", "app_killed", "launch_failed")) flags += reason
        if (reason != "crash" && crashInGame) flags += "crash"
        if (reason != "device_lost" && f?.inGame("device_lost") == true) flags += "device_lost"
        // A stall in the frame times alone is not one: the game's layer can give way to the X11 path for a while.
        if (e.zeroRunAfterLoadS >= 30 || e.longestZeroRunS >= 90) flags += "freeze"
        val capped = e.fpsLimit != null && e.fpsLimit in 1..29
        val mean = e.meanFpsAfterWarmup
        if ((mean != null && mean < 20.0 && !capped) || (e.low1 != null && e.low1 < 8.0 && (mean ?: 0.0) >= 30.0)) flags += "low_fps"
        if (f?.inGame("oom") == true || e.appKilled?.reason == "LOW_MEMORY") flags += "oom"
        if (f?.inGame("hidden") == true) flags += "hidden_paced"
        if (f?.inGame("x11_copy") == true) flags += "x11_copy"
        if (f?.software == true) flags += "software"
        if (f?.has("compiler") == true) flags += "compiler"
        if ((e.at60Thermal ?: 0.0) > 0.30) flags += "at60_thermal"
        if (e.marked) flags += "marked"
        // Upstream's rev 1 packs have no pool: Escape from Duckov went from about 98 to 72 fps on one.
        if (e.semPool == "none" || e.semPool == null && f?.has("no_sempool") == true) flags += "no_sempool"
        return Exit(
            reason, e.steamExitCode, signal ?: vortekSignal.takeIf { reason == "vortek_exit" }, e.vortekExit,
            e.appKilled, detail, flags.distinct(),
        )
    }

    /** DXVK naming the feature it lacks says more than the factory failing after it. */
    private fun missingFeature(f: LogSliceScanner.Findings?): String? {
        val lines = f?.errors?.get("unsupported")?.first.orEmpty().filter { line -> f?.gameLabels?.any { line.startsWith("$it: ") } == true }
        return lines.firstOrNull { it.contains("Skipping:") } ?: lines.firstOrNull() ?: f?.firstGameLine("unsupported")
    }

    /** How long a session folder is kept for a play's flags (ms), and why; null when it needs nothing beyond the newest ten. */
    fun keepFor(flags: Collection<String>): Pair<Long, List<String>>? {
        val day = 24L * 3600 * 1000
        val why = flags.filter { it in KEEP_DAYS }
        if (why.isEmpty()) return null
        return why.maxOf { KEEP_DAYS.getValue(it) } * day to why
    }

    val KEEP_DAYS = mapOf(
        "crash" to 14L, "device_lost" to 14L, "vortek_exit" to 14L, "freeze" to 14L, "app_killed" to 14L,
        "launch_failed" to 14L, "oom" to 14L, "compiler" to 14L, "low_fps" to 7L, "software" to 7L, "marked" to 30L,
    )
}
