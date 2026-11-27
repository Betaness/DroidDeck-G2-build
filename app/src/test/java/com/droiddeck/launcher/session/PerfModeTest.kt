package com.droiddeck.launcher.session

import android.view.Display
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowDisplayManager

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [31, 34], manifest = Config.NONE)
class PerfModeTest {
    private fun mode(id: Int, hz: Float, width: Int = 1080): Display.Mode =
        ShadowDisplayManager.ModeBuilder.modeBuilder(id)
            .setWidth(width).setHeight(2520).setRefreshRate(hz).build()

    @Test fun sixtyHzMirrorChoosesSixtyEvenThoughOneTwentyHasMatchingCadence() {
        val low = mode(1, 60f)
        val high = mode(2, 120f)
        assertEquals(low, PerfMode.selectMode(arrayOf(low, high), high, 60, 60f))
        assertEquals(high, PerfMode.selectMode(arrayOf(low, high), low, 0, Float.POSITIVE_INFINITY))
    }

    @Test fun aLowerGameLimitDoesNotOverrideTheGlassesRefreshCeiling() {
        val modes = arrayOf(mode(1, 60f), mode(2, 90f), mode(3, 120f))
        assertEquals(modes[0], PerfMode.selectMode(modes, modes[2], 30, 60f))
        assertEquals(modes[1], PerfMode.selectMode(modes, modes[2], 45, 90f))
    }

    @Test fun aGameCapGetsARefreshRateItDividesOnA144HzPanel() {
        val modes = arrayOf(mode(1, 60f), mode(2, 90f), mode(3, 120f), mode(4, 144f))
        val top = modes[3]
        assertEquals(modes[2], PerfMode.selectMode(modes, top, GameFpsCap.forMode(SessionPrefs.FPS_UNPACED, 60), Float.POSITIVE_INFINITY))
        assertEquals(modes[2], PerfMode.selectMode(modes, top, 40, Float.POSITIVE_INFINITY))
        assertEquals(modes[2], PerfMode.selectMode(modes, top, 30, Float.POSITIVE_INFINITY))
        assertEquals(modes[3], PerfMode.selectMode(modes, top, 72, Float.POSITIVE_INFINITY))
        assertEquals(modes[3], PerfMode.selectMode(modes, top, 48, Float.POSITIVE_INFINITY))
        assertEquals(modes[1], PerfMode.selectMode(modes, top, 45, Float.POSITIVE_INFINITY))
        assertEquals(modes[3], PerfMode.selectMode(modes, top, GameFpsCap.forMode(SessionPrefs.FPS_UNPACED, 0), Float.POSITIVE_INFINITY))
        assertEquals(modes[3], PerfMode.selectMode(modes, top, 50, Float.POSITIVE_INFINITY))
    }

    @Test fun selectionPreservesResolutionAndUsesTheLowestModeIfTheCapIsUnsupported() {
        val low = mode(1, 90f)
        val high = mode(2, 120f)
        val otherResolution = mode(3, 60f, 720)
        assertEquals(low, PerfMode.selectMode(arrayOf(low, high, otherResolution), high, 60, 60f))
    }

    @Test fun anotherModeWithTheRequestedRefreshRateKeepsTheRequestInsteadOfSwitchingBackAndForth() {
        val sixty = mode(2, 60f)
        val requested = mode(1, 120f)
        val reported = mode(255, 120f, 1440)
        val modes = arrayOf(sixty, requested, reported)
        assertEquals(reported, PerfMode.selectMode(modes, reported, 0, Float.POSITIVE_INFINITY))
        assertFalse(PerfMode.needsChange(modes, requested.modeId, reported))
        assertFalse(PerfMode.needsChange(modes, reported.modeId, requested))
    }

    @Test fun aDifferentRefreshRateNoRequestOrAMissingModeStillChangesTheMode() {
        val sixty = mode(2, 60f)
        val oneTwenty = mode(1, 120f)
        val modes = arrayOf(sixty, oneTwenty)
        assertTrue(PerfMode.needsChange(modes, sixty.modeId, oneTwenty))
        assertTrue(PerfMode.needsChange(modes, oneTwenty.modeId, sixty))
        assertTrue(PerfMode.needsChange(modes, 0, oneTwenty))
        assertTrue(PerfMode.needsChange(modes, 99, oneTwenty))
    }
}
