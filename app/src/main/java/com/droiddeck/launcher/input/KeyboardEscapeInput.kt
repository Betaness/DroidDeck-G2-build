package com.droiddeck.launcher.input

import android.view.KeyEvent

/**
 * [mali-g2] A keyboard's Esc that Android hands over as Back goes to the game as Esc, the whole press.
 *
 * HyperOS (Xiaomi Pad 9 Pro Max, Android 17) turns the Esc key of every external keyboard into KEYCODE_BACK
 * before any app sees it, keeping the key's scan code KEY_ESC (system_server: `notifyKey ... keyCode=0x4,
 * scanCode=0x1`, although Generic.kl maps key 1 to ESCAPE); upstream #2053 says many keyboards do the same.
 * #2053 sends that form to the guest as Esc ([GuestKeys.isKeyboardEscape]), but only at the end of
 * SessionActivity.dispatchKeyEvent, after BackQamInput, which in a Steam session takes every Back first: one
 * Esc opened the session drawer half a second later, two or a held Esc pressed the Deck's QAM so Steam opened
 * its quick access menu, and neither the game nor Steam ever got Esc.
 *
 * SessionActivity asks this before MouseBackInput and BackQamInput, and after the user's own physical bindings
 * and shortcuts, which can still give the key another job. Only Back with the Esc scan code is taken: the back
 * gesture and navigation bar (scan code 0), a controller's Back, a separate Back key (KEY_BACK, 158) and a
 * fallback Back keep upstream's Back (in a Steam session the drawer on a tap, the QAM on a double tap or hold).
 *
 * [sendEscape] presses or releases the guest's Esc for the device that pressed it. A press that started while
 * the game could take input sends its release and swallows its repeats even if a menu opens meanwhile; one
 * that started under a menu is left to the menu, release included.
 */
class KeyboardEscapeInput(
    private val sendEscape: (deviceId: Int, down: Boolean) -> Unit,
    private val isController: (KeyEvent) -> Boolean = PadBridge::isControllerKeyEvent,
) {
    private val held = mutableSetOf<Int>()

    /** True when [event] belonged to the guest's Esc; [toGuest] is whether the game can take keys right now. */
    fun onKeyEvent(event: KeyEvent, toGuest: Boolean): Boolean {
        if (event.keyCode != KeyEvent.KEYCODE_BACK) return false
        val device = event.deviceId
        return when (event.action) {
            KeyEvent.ACTION_DOWN -> {
                if (event.repeatCount > 0) return device in held
                // A new press while one is still held lost its release somewhere: end that one first.
                if (held.remove(device)) sendEscape(device, false)
                if (!toGuest || !isEscape(event, isController(event))) return false
                held += device
                sendEscape(device, true)
                true
            }
            KeyEvent.ACTION_UP -> {
                if (!held.remove(device)) return false
                sendEscape(device, false)
                true
            }
            else -> device in held
        }
    }

    fun onDeviceRemoved(deviceId: Int) {
        if (held.remove(deviceId)) sendEscape(deviceId, false)
    }

    companion object {
        const val KEY_ESC = 1

        /** Back carrying the Esc key's scan code from a keyboard, not a fallback Android made for an unhandled key. */
        fun isEscape(event: KeyEvent, fromController: Boolean): Boolean =
            GuestKeys.isKeyboardEscape(event.keyCode, event.scanCode) &&
                (event.flags and KeyEvent.FLAG_FALLBACK) == 0 && !fromController
    }
}
