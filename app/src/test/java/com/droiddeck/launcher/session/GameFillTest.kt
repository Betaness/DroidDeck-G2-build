package com.droiddeck.launcher.session

import android.content.Context
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class GameFillTest {
    private val context: Context get() = RuntimeEnvironment.getApplication()
    private val session get() = context.getSharedPreferences("session", Context.MODE_PRIVATE)

    @Before fun clear() {
        session.edit().clear().commit()
    }

    @Test fun stretchIsTheDefaultForEveryGame() {
        assertEquals(GameFill.STRETCH, SessionPrefs.gameFill(context))
        assertNull(SessionPrefs.gameFillFor(context, "2074800"))
        assertEquals(emptyMap<String, String>(), SessionPrefs.gameFillGames(context))
    }

    @Test fun theGeneralChoiceIsKeptAndUnknownValuesFallBackToStretch() {
        SessionPrefs.setGameFill(context, GameFill.ASPECT)
        assertEquals(GameFill.ASPECT, SessionPrefs.gameFill(context))
        SessionPrefs.setGameFill(context, GameFill.OFF)
        assertEquals(GameFill.OFF, SessionPrefs.gameFill(context))
        SessionPrefs.setGameFill(context, "zoom")
        assertEquals(GameFill.STRETCH, SessionPrefs.gameFill(context))
        session.edit().putString("gameFill", "integer").commit()
        assertEquals(GameFill.STRETCH, SessionPrefs.gameFill(context))
    }

    @Test fun gamesHaveTheirOwnUntilItIsCleared() {
        SessionPrefs.setGameFillFor(context, "2074800", GameFill.OFF)
        SessionPrefs.setGameFillFor(context, "440", GameFill.ASPECT)
        assertEquals(mapOf("2074800" to GameFill.OFF, "440" to GameFill.ASPECT), SessionPrefs.gameFillGames(context))
        assertEquals(GameFill.OFF, SessionPrefs.gameFillFor(context, "2074800"))
        SessionPrefs.setGameFillFor(context, "440", null)
        assertNull(SessionPrefs.gameFillFor(context, "440"))
        SessionPrefs.clearGameFillFor(context, "2074800")
        assertEquals(emptyMap<String, String>(), SessionPrefs.gameFillGames(context))
        assertEquals(GameFill.STRETCH, SessionPrefs.gameFill(context))
    }

    @Test fun onlyRealGamesAndChoicesAreStored() {
        assertThrows(IllegalArgumentException::class.java) { SessionPrefs.setGameFillFor(context, "", GameFill.OFF) }
        assertThrows(IllegalArgumentException::class.java) { SessionPrefs.setGameFillFor(context, "steam", GameFill.OFF) }
        assertThrows(IllegalArgumentException::class.java) { SessionPrefs.setGameFillFor(context, "440", "zoom") }
        session.edit().putString("gameFillGames", """{"440":"aspect","abc":"off","570":"zoom","0":"off","":"off"}""").commit()
        assertEquals(mapOf("440" to GameFill.ASPECT), SessionPrefs.gameFillGames(context))
        session.edit().putString("gameFillGames", "not json").commit()
        assertEquals(emptyMap<String, String>(), SessionPrefs.gameFillGames(context))
    }

    @Test fun theCompositorGetsModesByAppId() {
        val table = GameFill.table(true, GameFill.ASPECT,
            mapOf("2074800" to GameFill.OFF, "440" to GameFill.STRETCH, "4294967295" to GameFill.ASPECT, "x" to GameFill.OFF))
        assertEquals(GameFill.NATIVE_ASPECT, table.defaultMode)
        assertEquals(listOf(440, 2074800, -1), table.apps.toList())
        assertEquals(listOf(GameFill.NATIVE_STRETCH, GameFill.NATIVE_OFF, GameFill.NATIVE_ASPECT), table.modes.toList())
        assertEquals(GameFill.NATIVE_STRETCH, GameFill.table(true, "zoom", emptyMap()).defaultMode)
        assertEquals(GameFill.NATIVE_STRETCH, GameFill.nativeMode(null))
        assertEquals(GameFill.NATIVE_OFF, GameFill.nativeMode(GameFill.OFF))
    }

    @Test fun outsideSteamSessionsAndWithCardboardEverythingIsOff() {
        val table = GameFill.table(false, GameFill.STRETCH, mapOf("440" to GameFill.ASPECT))
        assertEquals(GameFill.NATIVE_OFF, table.defaultMode)
        assertEquals(0, table.apps.size)
        assertEquals(0, table.modes.size)
    }
}
