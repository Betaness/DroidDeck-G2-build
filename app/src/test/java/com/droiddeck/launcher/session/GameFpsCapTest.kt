package com.droiddeck.launcher.session

import org.junit.Assert.assertEquals
import org.junit.Test

class GameFpsCapTest {
    @Test fun theLastBlFpsLineIsTheCap() {
        assertEquals(60, GameFpsCap.parse(listOf("BL_FPS=40", "DXVK_HUD=fps", "BL_FPS=60")))
        assertEquals(72, GameFpsCap.parse(listOf("BL_FPS= 72 ")))
    }

    @Test fun noRateMeansNoCap() {
        for (value in listOf("", "0", "-30", "sixty", "60.5", "5000")) assertEquals(value, 0, GameFpsCap.parse(listOf("BL_FPS=$value")))
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
}
