package com.droiddeck.launcher.session

import android.hardware.display.DisplayManager
import android.view.Display
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.shadow.api.Shadow
import org.robolectric.shadows.ShadowDisplay
import org.robolectric.shadows.ShadowDisplayManager

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [31, 34], manifest = Config.NONE, shadows = [SessionRefreshRateTest.ModeDisplay::class])
class SessionRefreshRateTest {
    private val hz144 = mode(1, 144f)
    private val hz120 = mode(2, 120f)
    private val hz60 = mode(3, 60f)

    @Test fun aPerAppFrameRateOverrideDoesNotLowerTheSessionRate() {
        val (display, panel) = display(hz144)
        panel.setRefreshRate(48f)
        assertEquals(48f, display.refreshRate, 0.01f)
        assertEquals(144f, PerfMode.sessionHz(display, 0), 0.01f)
        assertEquals(144f, PerfMode.sessionHz(display, hz144.modeId), 0.01f)
        assertEquals(" (app frame rate 48 Hz, not followed)", PerfMode.appRateNote(display))
    }

    @Test fun theLowerOfTheRequestedAndTheCurrentModeIsUsed() {
        val (display, panel) = display(hz120)
        panel.setRefreshRate(40f)
        assertEquals(120f, PerfMode.sessionHz(display, hz144.modeId), 0.01f)
        assertEquals(60f, PerfMode.sessionHz(display, hz60.modeId), 0.01f)
        assertEquals(120f, PerfMode.sessionHz(display, 99), 0.01f)
    }

    @Test fun noOverrideAddsNothingToTheLog() {
        val (display, panel) = display(hz144)
        panel.setRefreshRate(144f)
        assertEquals("", PerfMode.appRateNote(display))
        assertEquals(0f, PerfMode.sessionHz(null, 0), 0.01f)
    }

    private fun mode(id: Int, hz: Float): Display.Mode =
        ShadowDisplayManager.ModeBuilder.modeBuilder(id).setWidth(3408).setHeight(2272).setRefreshRate(hz).build()

    private fun display(current: Display.Mode): Pair<Display, ModeDisplay> {
        val id = ShadowDisplayManager.addDisplay("w1704dp-h1136dp")
        val display = RuntimeEnvironment.getApplication().getSystemService(DisplayManager::class.java).getDisplay(id)
        val panel = Shadow.extract<ModeDisplay>(display).apply {
            this.current = current
            supported = arrayOf(hz144, hz120, hz60)
        }
        return display to panel
    }

    @Implements(Display::class)
    class ModeDisplay : ShadowDisplay() {
        var current: Display.Mode? = null
        var supported = emptyArray<Display.Mode>()

        @Implementation protected fun getMode(): Display.Mode =
            current ?: ShadowDisplayManager.ModeBuilder.modeBuilder(1).setWidth(1080).setHeight(1920).setRefreshRate(60f).build()

        @Implementation protected fun getSupportedModes(): Array<Display.Mode> = supported.ifEmpty { arrayOf(getMode()) }
    }
}
