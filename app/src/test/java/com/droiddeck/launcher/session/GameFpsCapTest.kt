package com.droiddeck.launcher.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GameFpsCapTest {
    @Test fun theLastBlFpsLineIsTheCap() {
        assertEquals(60, GameFpsCap.parse(listOf("BL_FPS=40", "DXVK_HUD=fps", "BL_FPS=60")))
        assertEquals(72, GameFpsCap.parse(listOf("BL_FPS=72")))
    }

    /** droiddeck-game-env takes BL_FPS only when it is all digits (frame_rate_limit), so no other value caps a game. */
    @Test fun aValueGameEnvDoesNotTakeIsNoCap() {
        for (value in listOf("", "0", "-30", "sixty", "60.5", "5000", " 72", "72 ", "+60", "6 0"))
            assertEquals(value, 0, GameFpsCap.parse(listOf("BL_FPS=$value")))
        assertEquals(0, GameFpsCap.parse(emptyList()))
    }

    @Test fun aGpuModeOtherThanVortekTurnsTheCapOff() {
        assertEquals(0, GameFpsCap.parse(listOf("BL_FPS=60", "BL_GPU_MODE=turnip")))
        assertEquals(60, GameFpsCap.parse(listOf("BL_GPU_MODE=" + VortekServerComponent.MODE, "BL_FPS=60")))
    }

    @Test fun theCompositorsOwnLimitComesFirst() {
        assertEquals(45, GameFpsCap.forMode(45, 60))
        assertEquals(60, GameFpsCap.forMode(SessionPrefs.FPS_UNPACED, 60))
        assertEquals(60, GameFpsCap.forMode(0, 60))
        assertEquals(0, GameFpsCap.forMode(SessionPrefs.FPS_UNPACED, 0))
    }

    /** The cap picks the panel mode only once the game's frames hold it: games it does not reach keep the top mode. */
    @Test fun theCapCountsOnceTheGameHoldsIt() {
        val hold = GameFpsCap.Hold(60)
        assertFalse(hold.held)
        assertNull(hold.sample(59.0))
        assertNull(hold.sample(61.0))
        assertEquals(true, hold.sample(60.0))
        assertTrue(hold.held)
        assertNull(hold.sample(60.0))
    }

    @Test fun aGameFasterThanTheCapLetsGo() {
        val hold = GameFpsCap.Hold(60)
        repeat(3) { hold.sample(60.0) }
        assertNull(hold.sample(85.0))
        assertNull(hold.sample(86.0))
        assertEquals(false, hold.sample(72.0))
        assertFalse(hold.held)
        assertNull(hold.sample(72.0))
    }

    @Test fun slowerFramesAndPausesChangeNothing() {
        val hold = GameFpsCap.Hold(60)
        repeat(2) { hold.sample(60.0) }
        assertNull(hold.sample(41.0))
        assertNull(hold.sample(60.0))
        assertNull(hold.sample(60.0))
        assertEquals(true, hold.sample(60.0))
        for (rate in listOf(0.0, 30.0, 55.0, 85.0, 90.0, 0.0, 85.0)) assertNull(rate.toString(), hold.sample(rate))
        assertTrue(hold.held)
    }

    @Test fun anUncappedGameNeverPicksTheMode() {
        val hold = GameFpsCap.Hold(60)
        for (rate in listOf(84.0, 87.0, 61.6, 86.0, 85.0, 58.3, 88.0, 90.0)) assertNull(hold.sample(rate))
        assertFalse(hold.held)
        assertNull(GameFpsCap.Hold(0).sample(0.0))
    }
}
