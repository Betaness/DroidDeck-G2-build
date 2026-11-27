package com.droiddeck.launcher.session

import android.view.Display
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class PresentCadenceGateTest {
    @Test fun runsOnTheBuiltInScreenFromAndroid13() {
        assertNull(PresentCadenceGate.waitsFor(33, false, false, Display.DEFAULT_DISPLAY))
        assertNull(PresentCadenceGate.waitsFor(36, false, false, Display.DEFAULT_DISPLAY))
        assertNotNull(PresentCadenceGate.waitsFor(32, false, false, Display.DEFAULT_DISPLAY))
    }

    /** The tick follows the built-in panel, so a session moved to an external monitor waits (upstream #2743). */
    @Test fun waitsWhileTheSessionIsOnAnotherDisplay() {
        assertEquals("the session is on display 2, not the built-in screen whose refreshes it follows",
            PresentCadenceGate.waitsFor(34, false, false, 2))
    }

    @Test fun waitsWhileTheGameIsOnTheGlassesOrTheBottomScreen() {
        assertEquals("the game is shown on the glasses", PresentCadenceGate.waitsFor(34, true, false, Display.DEFAULT_DISPLAY))
        assertEquals("the game is shown on the bottom screen", PresentCadenceGate.waitsFor(34, false, true, Display.DEFAULT_DISPLAY))
    }
}
