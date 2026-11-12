package com.droiddeck.launcher.input

import android.hardware.input.InputManager
import android.os.Looper
import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import androidx.compose.runtime.MutableState
import com.droiddeck.launcher.SessionActivity
import com.droiddeck.launcher.session.LoadingState
import com.droiddeck.launcher.session.SessionService
import com.droiddeck.launcher.session.SessionState
import com.droiddeck.launcher.wayland.CompositorHost
import java.io.File
import java.time.Duration
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.shadows.InputDeviceBuilder
import org.robolectric.util.ReflectionHelpers

/**
 * [mali-g2] A keyboard Esc that HyperOS reports as Back, through SessionActivity.dispatchKeyEvent in a Steam
 * session with the Deck pad: the case where BackQamInput used to open the drawer or the QAM instead.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], manifest = Config.NONE, shadows = [JoyConPadTest.RecordingWriter::class,
    PadBridgeRoutingTest.RegisteredKeyEvent::class, SessionInputPointerTest.CompositorShadow::class])
class SessionKeyboardEscapeTest {
    private val activity = Robolectric.buildActivity(SessionActivity::class.java).get()
    private val bridge = PadBridge(File(activity.cacheDir, "keyboard-escape"))
    private val events = mutableListOf<Pair<Int, Boolean>>()
    private val router = InputRouter(object : InputOutput {
        override fun key(code: Int, down: Boolean) { events += code to down }
        override fun button(code: Int, down: Boolean) {}
        override fun pointer(dx: Float, dy: Float) {}
        override fun scroll(steps: Int) {}
        override fun command(command: InputCommand) {}
        override fun requestMotion() {}
    })
    private val bindings = PhysicalInputBindings(router)
    private val previousMode = SessionState.mode
    private val previousRunning = SessionState.running
    private val previousFrame = SessionState.firstFrameSeen
    private val previousDeck = SessionState.deckPad
    private val previousSuspended = SessionState.suspended
    private val previousStarted = CompositorHost.isStarted

    @Before fun setUp() {
        SessionState.mode = SessionService.MODE_STEAM
        SessionState.running = true
        SessionState.firstFrameSeen = true
        SessionState.deckPad = true
        SessionState.suspended = false
        ReflectionHelpers.setStaticField(FakeInputWriter::class.java, "padType", FakeInputWriter.PAD_TYPE_DECK)
        ReflectionHelpers.setStaticField(CompositorHost::class.java, "started", true)
        ReflectionHelpers.setField(activity, "padBridge", bridge)
        ReflectionHelpers.setField(activity, "physicalInputBindings", bindings)
        ReflectionHelpers.setField(activity, "loading", LoadingState(activity).apply { visible = false })
        ReflectionHelpers.getField<MutableState<Boolean>>(activity, "resumed\$delegate").value = true
        val keyboard = InputDeviceBuilder.newBuilder().setId(KEYBOARD).setName("Cube Pocket Keyboard 2")
            .setDescriptor("cube-keyboard").setSources(InputDevice.SOURCE_KEYBOARD)
            .setKeyboardType(InputDevice.KEYBOARD_TYPE_ALPHABETIC).build()
        shadowOf(activity.getSystemService(InputManager::class.java)).addInputDevice(keyboard)
        JoyConPadTest.RecordingWriter.pads.clear()
        bridge.setInputRouter(router)
        bridge.start()
        SessionInputPointerTest.CompositorShadow.keys.clear()
    }

    @After fun close() {
        ReflectionHelpers.getField<BackQamInput>(activity, "backQamInput").cancel()
        bindings.close()
        bridge.stop()
        ReflectionHelpers.setStaticField(CompositorHost::class.java, "started", previousStarted)
        SessionState.mode = previousMode
        SessionState.running = previousRunning
        SessionState.firstFrameSeen = previousFrame
        SessionState.deckPad = previousDeck
        SessionState.suspended = previousSuspended
    }

    @Test fun escTapReachesTheGameWithoutTheDrawerOrQuickAccess() {
        assertTrue(activity.dispatchKeyEvent(esc(true)))
        assertTrue(activity.dispatchKeyEvent(esc(false)))
        advance(600)
        assertFalse(drawerOpen())
        assertFalse(quickAccess())
        assertEquals(listOf(ESC to 1, ESC to 0), guestKeys())
    }

    @Test fun twoQuickEscPressesAreTwoEscPressesWithoutQuickAccess() {
        activity.dispatchKeyEvent(esc(true))
        activity.dispatchKeyEvent(esc(false))
        advance(150)
        activity.dispatchKeyEvent(esc(true))
        activity.dispatchKeyEvent(esc(false))
        assertFalse(quickAccess())
        advance(600)
        assertFalse(drawerOpen())
        assertEquals(listOf(ESC to 1, ESC to 0, ESC to 1, ESC to 0), guestKeys())
    }

    @Test fun holdingEscHoldsEscAndNotQuickAccess() {
        assertTrue(activity.dispatchKeyEvent(esc(true)))
        advance(800)
        for (repeat in 1..3) assertTrue(activity.dispatchKeyEvent(esc(true, repeat)))
        assertFalse(quickAccess())
        assertFalse(JoyConPadTest.RecordingWriter.pads[0]?.isDown(PadState.QAM) == true)
        assertEquals(listOf(ESC to 1), guestKeys())
        assertTrue(activity.dispatchKeyEvent(esc(false)))
        assertEquals(listOf(ESC to 1, ESC to 0), guestKeys())
        assertFalse(drawerOpen())
    }

    @Test fun aMenuOpeningMidPressStillReleasesEscInTheGame() {
        activity.dispatchKeyEvent(esc(true))
        ReflectionHelpers.getField<MutableState<Boolean>>(activity, "drawerOpenState\$delegate").value = true
        assertTrue(activity.dispatchKeyEvent(esc(false)))
        assertEquals(listOf(ESC to 1, ESC to 0), guestKeys())
    }

    @Test fun theBackGestureStillOpensTheDrawer() {
        assertTrue(activity.dispatchKeyEvent(back(true, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
            KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_VIRTUAL_HARD_KEY)))
        assertTrue(activity.dispatchKeyEvent(back(false, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
            KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_VIRTUAL_HARD_KEY)))
        advance(600)
        assertTrue(drawerOpen())
        assertTrue(guestKeys().isEmpty())
    }

    @Test fun aFallbackBackWithTheEscScanCodeStaysBack() {
        val flags = KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_FALLBACK
        assertTrue(activity.dispatchKeyEvent(back(true, KEYBOARD, 1, flags)))
        assertTrue(activity.dispatchKeyEvent(back(false, KEYBOARD, 1, flags)))
        advance(600)
        assertTrue(drawerOpen())
        assertTrue(guestKeys().isEmpty())
    }

    @Test fun aBindingOnTheEscKeyStillWins() {
        bindings.configure(listOf(PhysicalInputBinding(PhysicalInputKey.capture(esc(true))!!, InputAction.Keys(listOf(28)))))
        assertTrue(activity.dispatchKeyEvent(esc(true)))
        assertTrue(activity.dispatchKeyEvent(esc(false)))
        assertEquals(listOf(28 to true, 28 to false), events)
        assertTrue(guestKeys().isEmpty())
        assertFalse(drawerOpen())
    }

    private fun drawerOpen(): Boolean =
        ReflectionHelpers.getField<MutableState<Boolean>>(activity, "drawerOpenState\$delegate").value

    /** The Deck's QAM pressed by a double tap or by holding Back. */
    private fun quickAccess(): Boolean =
        ReflectionHelpers.getField<Boolean>(bridge, "qamTapPressed") || ReflectionHelpers.getField<Boolean>(bridge, "backQamPressed")

    private fun guestKeys() = SessionInputPointerTest.CompositorShadow.keys.toList()

    /** As logged on the Xiaomi Pad 9 Pro Max: `keyCode=0x4, scanCode=0x1, flags=0x8, source=0x101`. */
    private fun esc(down: Boolean, repeat: Int = 0) = back(down, KEYBOARD, 1, KeyEvent.FLAG_FROM_SYSTEM, repeat)

    private fun back(down: Boolean, device: Int, scan: Int, flags: Int, repeat: Int = 0) = KeyEvent(0L, 0L,
        if (down) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, repeat, 0, device, scan, flags,
        InputDevice.SOURCE_KEYBOARD)

    private fun advance(ms: Long) = shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms))

    private companion object {
        const val KEYBOARD = 31
        const val ESC = 1
    }
}
