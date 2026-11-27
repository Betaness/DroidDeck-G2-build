package com.droiddeck.launcher.session

import android.content.Context
import com.droiddeck.launcher.core.FileUtils

/**
 * [mali-g2] BL_FPS in droiddeck-env: droiddeck-game-env holds Proton games on Vortek and libhybris to it through DXVK
 * and vkd3d-proton (frame_rate_limit), in Steam sessions too, where the drawer offers no limit. It does not reach
 * every game (a limit of the game's own, Vulkan or OpenGL games, native programs), so the cap picks the panel mode
 * only while the game's frames hold it ([Hold]): then the mode is one the cap divides ([PerfMode.cadenceFits]), a
 * 60 fps cap gets 120 Hz rather than 144 Hz, where frames would alternate between 2 and 3 refreshes.
 */
object GameFpsCap {
    /** The cap the last BL_FPS line of [env] sets, 0 when none or not a rate game-env takes (only digits);
     *  BL_GPU_MODE other than Vortek's turns it off. */
    fun parse(env: List<String>): Int {
        val mode = env.lastOrNull { it.startsWith("BL_GPU_MODE=") }?.substringAfter('=')
        if (mode != null && mode != VortekServerComponent.MODE) return 0
        val value = env.lastOrNull { it.startsWith("BL_FPS=") }?.substringAfter('=') ?: return 0
        if (value.isEmpty() || !value.all { it in '0'..'9' }) return 0
        return value.toIntOrNull()?.takeIf { it in 1..1000 } ?: 0
    }

    /**
     * Whether the game's frames hold [cap]: [HOLD_SECONDS] seconds in a row at the cap (within [tolerance]) and it
     * holds, as many above it and it lets go. Slower seconds (a heavy scene, a pause, a menu) change nothing.
     */
    class Hold(private val cap: Int) {
        var held = false
            private set
        private var at = 0
        private var above = 0
        private val tolerance = maxOf(1.5, cap * 0.025)

        /** One second's game frame rate; the new state when it changed, else null. */
        fun sample(fps: Double): Boolean? {
            if (cap <= 0) return null
            when {
                fps > cap + tolerance -> { above++; at = 0 }
                fps >= cap - tolerance -> { at++; above = 0 }
                else -> { at = 0; above = 0 }
            }
            val next = when {
                !held && at >= HOLD_SECONDS -> true
                held && above >= HOLD_SECONDS -> false
                else -> return null
            }
            held = next
            at = 0
            above = 0
            return next
        }
    }

    const val HOLD_SECONDS = 3

    /** droiddeck-env as the session reads it (SessionService.extraEnv), on devices that run games through Vortek or libhybris. */
    fun read(context: Context): Int = runCatching {
        if (!com.droiddeck.launcher.core.DeviceSupport.ownMaliPath(context)) return 0
        val file = SessionService.envSwitchFile(context)?.takeIf { it.isFile } ?: return 0
        parse(FileUtils.readString(file)?.lines().orEmpty().map { it.trim() }
            .filter { it.isNotEmpty() && !it.startsWith("#") && it.contains('=') && !it.startsWith("=") })
    }.getOrDefault(0)

    /** The rate the panel mode has to divide: the compositor's own limit when there is one, else the games' cap. */
    fun forMode(outputLimit: Int, gameCap: Int): Int = if (outputLimit > 0) outputLimit else gameCap.coerceAtLeast(0)
}
