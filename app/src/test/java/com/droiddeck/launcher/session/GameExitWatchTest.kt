package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.GameExitWatch.Step
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Test

class GameExitWatchTest {
    private val script = listOf("App 1473480 (Running Script)")
    private val commit = listOf("App 730 (Committing)")

    private fun watch() = GameExitWatch(startBy = 1_200_000L, graceMs = 10_000L, holdMaxMs = 600_000L)

    private val idle = { emptyList<String>() }
    private val notAsked: () -> List<String> = { fail("Steam's work was read before the game closed"); emptyList() }

    /**
     * Polls every 2 s from [from] to [to] with the game gone, up to a step that ends the watch; returns the steps
     * that are not [Step.Wait], by time.
     */
    private fun GameExitWatch.gone(from: Long, to: Long, busy: (Long) -> List<String>): List<Pair<Long, Step>> {
        val steps = ArrayList<Pair<Long, Step>>()
        for (t in from..to step 2_000L) {
            val s = step(t, false) { busy(t) }
            if (s != Step.Wait) steps += t to s
            if (s == Step.Return || s is Step.Expired) break
        }
        return steps
    }

    @Test fun theClientIsAskedToExitTenSecondsAfterTheGameCloses() {
        val watch = watch()
        assertEquals(Step.Wait, watch.step(1_000L, false, notAsked))
        assertEquals(Step.Started, watch.step(3_000L, true, notAsked))
        assertEquals(Step.Wait, watch.step(5_000L, true, notAsked))
        assertEquals(listOf(17_000L to Step.Return), watch.gone(7_000L, 17_000L) { emptyList() })
    }

    @Test fun aGameThatNeverStartsIsNotWatchedAndSteamIsNotReadMeanwhile() {
        val watch = watch()
        assertEquals(Step.Wait, watch.step(1_200_000L, false, notAsked))
        assertEquals(Step.NotStarted, watch.step(1_200_002L, false, notAsked))
    }

    @Test fun anInstallScriptHoldsTheReturnOnceAndTheGraceStartsWhenItEnds() {
        val watch = watch()
        watch.step(1_000L, true, notAsked)
        val steps = watch.gone(3_000L, 60_000L) { t ->
            when {
                t < 21_000L -> script
                t < 31_000L -> commit + listOf("uninstall 9")
                else -> emptyList()
            }
        }
        assertEquals(listOf(
            3_000L to Step.Held(script, again = false),
            31_000L to Step.Done(script + commit + listOf("uninstall 9")),
            41_000L to Step.Return,
        ), steps)
    }

    @Test fun workThatStartsDuringTheGraceStartsTheGraceAgainAfterIt() {
        val watch = watch()
        watch.step(1_000L, true, notAsked)
        val steps = watch.gone(3_000L, 40_000L) { t -> if (t in 9_000L..13_000L || t in 19_000L..21_000L) commit else emptyList() }
        assertEquals(listOf(
            9_000L to Step.Held(commit, again = false),
            15_000L to Step.Done(commit),
            19_000L to Step.Held(commit, again = true),
            23_000L to Step.Done(commit),
            33_000L to Step.Return,
        ), steps)
    }

    @Test fun theHoldEndsAfterItsLimit() {
        val watch = watch()
        watch.step(1_000L, true, notAsked)
        val steps = watch.gone(3_000L, 700_000L) { t -> if (t in 101_000L..109_000L) emptyList() else script }
        assertEquals(listOf(
            3_000L to Step.Held(script, again = false),
            101_000L to Step.Done(script),
            111_000L to Step.Held(script, again = true),
            603_000L to Step.Expired(script),
        ), steps)
    }

    @Test fun aGameRunAgainDuringAHoldStartsOver() {
        val watch = watch()
        watch.step(1_000L, true, notAsked)
        assertEquals(listOf(3_000L to Step.Held(script, again = false)), watch.gone(3_000L, 500_000L) { script })
        assertEquals(Step.Wait, watch.step(502_000L, true, notAsked))
        assertEquals(listOf(514_000L to Step.Return), watch.gone(504_000L, 514_000L) { emptyList() })

        val again = watch()
        again.step(1_000L, true, notAsked)
        again.gone(3_000L, 500_000L) { script }
        again.step(502_000L, true, notAsked)
        assertEquals(listOf(
            504_000L to Step.Held(commit, again = false),
            1_104_000L to Step.Expired(commit),
        ), again.gone(504_000L, 1_104_000L) { commit })
    }

    @Test fun nothingIsHeldWhenSteamIsIdle() {
        val watch = watch()
        watch.step(1_000L, true, notAsked)
        assertEquals(listOf(13_000L to Step.Return), watch.gone(3_000L, 13_000L, { idle() }))
    }
}
