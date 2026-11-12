package com.droiddeck.launcher.input

import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/** [mali-g2] The routing decision for a keyboard Esc that Android reports as Back. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], manifest = Config.NONE)
class KeyboardEscapeInputTest {
    private val sent = mutableListOf<Pair<Int, Boolean>>()
    private var controller = false
    private val input = KeyboardEscapeInput({ device, down -> sent += device to down }) { controller }

    private fun key(action: Int, device: Int = 31, repeat: Int = 0, code: Int = KeyEvent.KEYCODE_BACK,
                    scan: Int = 1, flags: Int = KeyEvent.FLAG_FROM_SYSTEM, source: Int = InputDevice.SOURCE_KEYBOARD) =
        KeyEvent(0L, 0L, action, code, repeat, 0, device, scan, flags, source)

    /** What a USB or Bluetooth keyboard's Esc reaches the app as on HyperOS, as logged on a Xiaomi Pad 9 Pro Max. */
    private fun esc(action: Int, device: Int = 31, repeat: Int = 0) = key(action, device = device, repeat = repeat)

    /** What the navigation bar and back gesture send. */
    private fun navigationBar(action: Int) = key(action, device = KeyCharacterMap.VIRTUAL_KEYBOARD, scan = 0,
        flags = KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_VIRTUAL_HARD_KEY)

    @Test fun escReportedAsBackIsTheGamesEsc() {
        assertTrue(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = true))
        assertTrue(input.onKeyEvent(esc(KeyEvent.ACTION_UP), toGuest = true))
        assertEquals(listOf(31 to true, 31 to false), sent)
    }

    @Test fun holdingEscSwallowsRepeatsWithoutPressingAgain() {
        input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = true)
        repeat(3) { assertTrue(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN, repeat = it + 1), toGuest = true)) }
        input.onKeyEvent(esc(KeyEvent.ACTION_UP), toGuest = true)
        assertEquals(listOf(31 to true, 31 to false), sent)
    }

    @Test fun twoQuickPressesAreTwoEscPresses() {
        for (action in listOf(KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP, KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP)) {
            assertTrue(input.onKeyEvent(esc(action), toGuest = true))
        }
        assertEquals(listOf(31 to true, 31 to false, 31 to true, 31 to false), sent)
    }

    @Test fun theBackGestureAndNavigationBarStayBack() {
        for (action in listOf(KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP)) {
            assertFalse(input.onKeyEvent(navigationBar(action), toGuest = true))
        }
        assertTrue(sent.isEmpty())
    }

    @Test fun aSeparateBackKeyStaysBack() {
        for (action in listOf(KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP)) {
            assertFalse(input.onKeyEvent(key(action, scan = 158), toGuest = true))
        }
        assertTrue(sent.isEmpty())
    }

    @Test fun aControllerBackStaysBackWhateverItsScanCode() {
        controller = true
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = true))
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_UP), toGuest = true))
        assertTrue(sent.isEmpty())
    }

    @Test fun aFallbackBackStaysBack() {
        val flags = KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_FALLBACK
        assertFalse(input.onKeyEvent(key(KeyEvent.ACTION_DOWN, flags = flags), toGuest = true))
        assertFalse(input.onKeyEvent(key(KeyEvent.ACTION_UP, flags = flags), toGuest = true))
        assertTrue(sent.isEmpty())
    }

    @Test fun anEscapeKeyCodeIsLeftToTheUsualKeyPath() {
        assertFalse(input.onKeyEvent(key(KeyEvent.ACTION_DOWN, code = KeyEvent.KEYCODE_ESCAPE), toGuest = true))
        assertTrue(sent.isEmpty())
    }

    @Test fun escUnderAMenuIsLeftToTheMenu() {
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = false))
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN, repeat = 1), toGuest = false))
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_UP), toGuest = false))
        assertTrue(sent.isEmpty())
    }

    @Test fun aMenuOpeningMidPressStillReleasesEsc() {
        input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = true)
        assertTrue(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN, repeat = 1), toGuest = false))
        assertTrue(input.onKeyEvent(esc(KeyEvent.ACTION_UP), toGuest = false))
        assertEquals(listOf(31 to true, 31 to false), sent)
    }

    @Test fun aMenuClosingMidPressLeavesTheReleaseToTheMenu() {
        input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = false)
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN, repeat = 1), toGuest = true))
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_UP), toGuest = true))
        assertTrue(sent.isEmpty())
    }

    @Test fun aPressWhoseReleaseWasLostIsEndedByTheNextPress() {
        input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = true)
        assertTrue(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = true))
        assertEquals(listOf(31 to true, 31 to false, 31 to true), sent)
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = false))
        assertEquals(listOf(31 to true, 31 to false, 31 to true, 31 to false), sent)
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_UP), toGuest = false))
    }

    @Test fun eachKeyboardHoldsItsOwnPress() {
        input.onKeyEvent(esc(KeyEvent.ACTION_DOWN, device = 31), toGuest = true)
        input.onKeyEvent(esc(KeyEvent.ACTION_DOWN, device = 33), toGuest = true)
        input.onKeyEvent(esc(KeyEvent.ACTION_UP, device = 31), toGuest = true)
        assertTrue(input.onKeyEvent(esc(KeyEvent.ACTION_DOWN, device = 33, repeat = 1), toGuest = true))
        input.onKeyEvent(esc(KeyEvent.ACTION_UP, device = 33), toGuest = true)
        assertEquals(listOf(31 to true, 33 to true, 31 to false, 33 to false), sent)
    }

    @Test fun theKeyboardDisconnectingMidPressReleasesEsc() {
        input.onKeyEvent(esc(KeyEvent.ACTION_DOWN), toGuest = true)
        input.onDeviceRemoved(9)
        assertEquals(listOf(31 to true), sent)
        input.onDeviceRemoved(31)
        assertEquals(listOf(31 to true, 31 to false), sent)
        assertFalse(input.onKeyEvent(esc(KeyEvent.ACTION_UP), toGuest = true))
        assertEquals(listOf(31 to true, 31 to false), sent)
    }

    @Test fun onlyBackWithTheEscScanCodeCounts() {
        assertTrue(KeyboardEscapeInput.isEscape(esc(KeyEvent.ACTION_DOWN), fromController = false))
        val virtual = key(KeyEvent.ACTION_DOWN, device = KeyCharacterMap.VIRTUAL_KEYBOARD)
        assertTrue(KeyboardEscapeInput.isEscape(virtual, fromController = false))
        assertFalse(KeyboardEscapeInput.isEscape(esc(KeyEvent.ACTION_DOWN), fromController = true))
        assertFalse(KeyboardEscapeInput.isEscape(key(KeyEvent.ACTION_DOWN, scan = 0), fromController = false))
        assertFalse(KeyboardEscapeInput.isEscape(key(KeyEvent.ACTION_DOWN, scan = 158), fromController = false))
        assertFalse(KeyboardEscapeInput.isEscape(key(KeyEvent.ACTION_DOWN, flags = KeyEvent.FLAG_FALLBACK), fromController = false))
        assertFalse(KeyboardEscapeInput.isEscape(key(KeyEvent.ACTION_DOWN, code = KeyEvent.KEYCODE_ESCAPE), fromController = false))
    }
}
