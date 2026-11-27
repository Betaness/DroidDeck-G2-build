package com.droiddeck.launcher.session

import android.content.Context
import com.droiddeck.launcher.core.FileUtils

/**
 * [mali-g2] BL_FPS in droiddeck-env: droiddeck-game-env holds Vortek and libhybris games to it through DXVK and
 * vkd3d-proton (frame_rate_limit), in Steam sessions too, where the drawer offers no limit. The panel mode is chosen
 * so that it divides the cap ([PerfMode.cadenceFits]): a 60 fps cap gets 120 Hz rather than 144 Hz, where frames would
 * alternate between 2 and 3 refreshes.
 */
object GameFpsCap {
    /** The cap the last BL_FPS line of [env] sets, 0 when none or not a rate; BL_GPU_MODE other than Vortek's turns it off. */
    fun parse(env: List<String>): Int {
        val mode = env.lastOrNull { it.startsWith("BL_GPU_MODE=") }?.substringAfter('=')
        if (mode != null && mode != VortekServerComponent.MODE) return 0
        val value = env.lastOrNull { it.startsWith("BL_FPS=") }?.substringAfter('=')?.trim() ?: return 0
        return value.toIntOrNull()?.takeIf { it in 1..1000 } ?: 0
    }

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
