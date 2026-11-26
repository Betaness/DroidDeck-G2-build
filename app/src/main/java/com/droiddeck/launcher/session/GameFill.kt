package com.droiddeck.launcher.session

/**
 * [mali-g2] Game window fill: in sessions without gamescope the compositor shows a Steam game's window
 * that is smaller than the X screen over the whole output (waylandcomp/src/game_fill.h), as gamescope
 * scales the focused window. One setting for all games, and games can have their own.
 */
object GameFill {
    const val STRETCH = "stretch"
    const val ASPECT = "aspect"
    const val OFF = "off"
    const val DEFAULT = STRETCH
    val CHOICES = listOf(STRETCH, ASPECT, OFF)

    /** The compositor's mode (GF_MODE_*). */
    const val NATIVE_OFF = 0
    const val NATIVE_STRETCH = 1
    const val NATIVE_ASPECT = 2

    fun normalize(choice: String?): String? = choice?.takeIf { it in CHOICES }

    /**
     * Before this setting, the 16:9 game fit (stretch16x9) asked gamescope to keep proportions or stretch;
     * there is no gamescope on this path. An explicit keep becomes [ASPECT]; otherwise [DEFAULT].
     */
    fun fromStretch16x9(explicit: Boolean?): String = if (explicit == false) ASPECT else DEFAULT

    fun nativeMode(choice: String?): Int = when (normalize(choice) ?: DEFAULT) {
        OFF -> NATIVE_OFF
        ASPECT -> NATIVE_ASPECT
        else -> NATIVE_STRETCH
    }

    /** What the compositor is given: its default mode and the games (Steam app ids) with a mode of their own. */
    class Table(val defaultMode: Int, val apps: IntArray, val modes: IntArray)

    /**
     * [general] for every game and [games] (game profile scope -> choice) for those with their own. Inactive
     * (not a Steam session, or cardboard, whose pass draws the whole screen): off. App ids go as unsigned
     * 32-bit values, as the X window properties carry them.
     */
    fun table(active: Boolean, general: String, games: Map<String, String>): Table {
        if (!active) return Table(NATIVE_OFF, IntArray(0), IntArray(0))
        val own = games.mapNotNull { (scope, choice) ->
            val app = scope.toLongOrNull()?.takeIf { it in 1L..0xffffffffL } ?: return@mapNotNull null
            normalize(choice)?.let { app to nativeMode(it) }
        }.sortedBy { it.first }
        return Table(nativeMode(general), IntArray(own.size) { own[it].first.toInt() }, IntArray(own.size) { own[it].second })
    }
}
