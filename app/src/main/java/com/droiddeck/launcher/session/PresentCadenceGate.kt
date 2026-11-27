package com.droiddeck.launcher.session

import android.os.Build
import android.view.Display

/**
 * [mali-g2] Even frame pacing plans on the frame timeline of the compositor's tick (Choreographer on its own thread),
 * which follows the built-in panel's refreshes. It waits while the game is shown anywhere else.
 */
object PresentCadenceGate {
    /** Why pacing waits although it is switched on, null when it can run. */
    fun waitsFor(sdk: Int, glasses: Boolean, bottomScreen: Boolean, activityDisplayId: Int): String? = when {
        sdk < Build.VERSION_CODES.TIRAMISU -> "it needs Android 13"
        glasses -> "the game is shown on the glasses"
        bottomScreen -> "the game is shown on the bottom screen"
        activityDisplayId != Display.DEFAULT_DISPLAY ->
            "the session is on display $activityDisplayId, not the built-in screen whose refreshes it follows"
        else -> null
    }
}
