package com.droiddeck.launcher.gpu

import android.content.Context
import android.system.Os
import android.util.Log
import com.droiddeck.launcher.R
import com.droiddeck.launcher.core.DeviceSupport
import com.droiddeck.launcher.session.SessionState
import com.droiddeck.launcher.wayland.WaylandCompositor

object FrameGen {
    const val ENGINE_OFF = "off"
    const val ENGINE_DDFG = "ddfg"
    private const val LEGACY_ENGINE_WINFG = "winfg"
    const val ENGINE_LSFG = "lsfg"
    /** [mali-g2] ZeroFG on the zero-copy main window (waylandcomp/ZEROFG.md): ×2 only, two engines. */
    const val ENGINE_ZEROFG = "zerofg"
    const val ZEROFG_REALLY_ZERO = "reallyzero"
    const val ZEROFG_ZERO = "zero"
    val ZEROFG_MODES = listOf(ZEROFG_REALLY_ZERO, ZEROFG_ZERO)

    private const val DDFG_PERF_PRESET = 2
    private const val DDFG_MODEL = 4
    private const val FLOW_SCALE_DDFG = 0.60f
    private const val FLOW_SCALE_LSFG = 0.80f

    val ADAPTIVE_TARGETS = listOf(120, 90, 60)

    private const val PREFS = "frame_gen"

    /** [zeroFg]: ZeroFG's engine, set only with [ENGINE_ZEROFG]. */
    data class Mode(val engine: String, val multiplier: Int = 2, val target: Int = 0, val zeroFg: String = "") {
        companion object {
            val OFF = Mode(ENGINE_OFF)
        }
    }

    fun zeroFg(engine: String): Mode = Mode(ENGINE_ZEROFG, zeroFg = engine.takeIf { it in ZEROFG_MODES } ?: ZEROFG_REALLY_ZERO)

    /** The ZeroFG items of the frame-generation menu, in order. */
    val ZEROFG_MENU: List<Mode> = ZEROFG_MODES.map { zeroFg(it) }

    fun engine(context: Context): String = mode(context).engine

    fun mode(context: Context): Mode {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val engine = prefs.getString("engine", ENGINE_OFF).let { if (it == LEGACY_ENGINE_WINFG) ENGINE_DDFG else it ?: ENGINE_OFF }
        val target = prefs.getInt("target", 0)
        if (engine == ENGINE_ZEROFG) return zeroFg(prefs.getString("zerofg", ZEROFG_REALLY_ZERO) ?: ZEROFG_REALLY_ZERO)
        if (engine == ENGINE_LSFG && target in ADAPTIVE_TARGETS) return Mode(engine, target = target)
        return Mode(engine, prefs.getInt("multiplier", 2).coerceIn(2, 4))
    }

    fun set(context: Context, mode: Mode) {
        val edit = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString("engine", mode.engine).putInt("multiplier", mode.multiplier).putInt("target", mode.target)
        if (mode.engine == ENGINE_ZEROFG) edit.putString("zerofg", zeroFg(mode.zeroFg).zeroFg)
        edit.apply()
    }

    /** Why ZeroFG cannot run on this device's GPU, as the last session found; null: nothing known against it. */
    fun zeroFgProblem(context: Context): String? =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("zerofg_problem", null)?.takeIf { it.isNotBlank() }

    fun noteZeroFgProblem(context: Context, problem: String?) {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val value = problem?.takeIf { it.isNotBlank() }
        if (prefs.getString("zerofg_problem", null) != value) prefs.edit().putString("zerofg_problem", value).apply()
    }

    /** Why ZeroFG stopped in this session, or the size it could not be built for, as the compositor last said;
     *  null: nothing. Forgotten when a session starts (CompositorHost.newSession). */
    @Volatile var zeroFgFailure: String? = null

    /** ZeroFG runs only on frames shown zero-copy: this session's path, or outside a session this device's. */
    fun zeroFgPathOk(context: Context): Boolean =
        if (SessionState.running) SessionState.zeroCopy != false else DeviceSupport.ownMaliPath(context)

    /** The compositor reads these when ZeroFG turns on (waylandcomp/ZEROFG.md). */
    val ZEROFG_ENV = listOf(
        "DROIDDECK_ZEROFG_MODE", "DROIDDECK_ZEROFG_PRIORITY", "DROIDDECK_ZEROFG_DPT", "DROIDDECK_ZEROFG_VOTE",
        "DROIDDECK_ZEROFG_MARGIN_MS", "DROIDDECK_ZEROFG_LATCH_MS", "DROIDDECK_ZEROFG_BACKEND", "DROIDDECK_ZEROFG_TRACE",
    )

    /** Each ZeroFG knob's value in [env] (KEY=value lines, the last one counts); null: not given. */
    internal fun zeroFgEnv(env: List<String>): Map<String, String?> =
        ZEROFG_ENV.associateWith { key -> env.lastOrNull { it.startsWith("$key=") }?.substringAfter('=') }

    /** Sets the ZeroFG knobs of [env] in this process's environment and clears the others. */
    fun applyZeroFgEnv(env: List<String>) {
        for ((key, value) in zeroFgEnv(env)) {
            runCatching { if (value != null) Os.setenv(key, value, true) else Os.unsetenv(key) }
                .onFailure { Log.w(TAG, key, it) }
        }
    }

    internal fun nativeZeroFgMode(mode: Mode): Int =
        if (mode.zeroFg == ZEROFG_ZERO) WaylandCompositor.ZEROFG_MODE_ZERO else WaylandCompositor.ZEROFG_MODE_REALLY_ZERO

    fun label(context: Context): String = label(context, mode(context))

    fun label(context: Context, mode: Mode): String = when {
        mode.engine == ENGINE_ZEROFG && mode.zeroFg == ZEROFG_ZERO -> context.getString(R.string.frame_gen_zerofg_zero)
        mode.engine == ENGINE_ZEROFG -> context.getString(R.string.frame_gen_zerofg_really)
        mode.engine == ENGINE_DDFG -> context.getString(R.string.frame_gen_ddfg, mode.multiplier)
        mode.engine == ENGINE_LSFG && mode.target > 0 -> context.getString(R.string.frame_gen_lsfg_adaptive, mode.target)
        mode.engine == ENGINE_LSFG -> context.getString(R.string.frame_gen_lsfg, mode.multiplier)
        else -> context.getString(R.string.frame_gen_off)
    }

    fun apply(context: Context, refreshHz: Float, peakHz: Float = refreshHz): String? =
        apply(context, mode(context), refreshHz, peakHz)

    /** [peakHz]: the highest rate the session lets the panel run at; ZeroFG's frame-rate vote is a whole
     *  fraction of it, so a vote that lowered the panel can raise it again. */
    fun apply(context: Context, mode: Mode, refreshHz: Float, peakHz: Float = refreshHz): String? {
        when (mode.engine) {
            ENGINE_LSFG -> {
                val status = Lossless.sync(context)
                val cache = Lossless.cacheFile(context)
                if (status != LsfgNative.STATUS_OK || cache == null) {
                    WaylandCompositor.nativeSetFrameGenArmed(false, 0, 0)
                    val problem = context.getString(
                        if (status == LsfgNative.STATUS_NOT_INSTALLED) R.string.lsfg_problem_missing else R.string.lsfg_problem_failed,
                    )
                    Log.w(TAG, "$problem (status $status)")
                    return problem
                }
                WaylandCompositor.nativeSetFrameGenEngine(WaylandCompositor.FG_ENGINE_LSFG)
                WaylandCompositor.nativeSetLsfgCachePath(cache.absolutePath)
                WaylandCompositor.nativeSetFrameGenTuning(FLOW_SCALE_LSFG, refreshHz)
                WaylandCompositor.nativeSetFrameGenArmed(true, mode.multiplier, mode.target)
            }
            ENGINE_ZEROFG -> {
                WaylandCompositor.nativeSetFrameGenEngine(WaylandCompositor.FG_ENGINE_ZEROFG)
                WaylandCompositor.nativeSetZeroFgMode(nativeZeroFgMode(mode))
                // ZeroFG has no flow scale; the refresh rate paces its frames.
                WaylandCompositor.nativeSetFrameGenTuning(FLOW_SCALE_LSFG, refreshHz)
                WaylandCompositor.nativeSetZeroFgPeakHz(maxOf(peakHz, refreshHz))
                WaylandCompositor.nativeSetFrameGenArmed(true, 2, 0)
                val problem = runCatching { WaylandCompositor.nativeZeroFgProblem() }.getOrNull()
                if (problem != null) noteZeroFgProblem(context, problem)
                if (!problem.isNullOrBlank()) {
                    Log.w(TAG, "ZeroFG: $problem")
                    return context.getString(R.string.zerofg_problem, problem)
                }
                if (SessionState.running && SessionState.zeroCopy == false) {
                    Log.w(TAG, "ZeroFG: this session does not show games zero-copy")
                    return context.getString(R.string.zerofg_needs_zero_copy)
                }
                val failure = runCatching { WaylandCompositor.nativeZeroFgFailure() }.getOrNull()
                zeroFgFailure = failure
                if (failure != null) {
                    Log.w(TAG, "ZeroFG: $failure")
                    return context.getString(R.string.zerofg_failed, failure)
                }
            }
            ENGINE_DDFG -> {
                WaylandCompositor.nativeSetFrameGenEngine(WaylandCompositor.FG_ENGINE_DDFG)
                WaylandCompositor.nativeSetDdFgTuning(DDFG_MODEL, DDFG_PERF_PRESET)
                WaylandCompositor.nativeSetFrameGenTuning(FLOW_SCALE_DDFG, refreshHz)
                WaylandCompositor.nativeSetFrameGenArmed(true, mode.multiplier, 0)
            }
            else -> WaylandCompositor.nativeSetFrameGenArmed(false, 0, 0)
        }
        Log.i(TAG, "frame generation: $mode at ${refreshHz}Hz")
        return null
    }

    private const val TAG = "FrameGen"
}
