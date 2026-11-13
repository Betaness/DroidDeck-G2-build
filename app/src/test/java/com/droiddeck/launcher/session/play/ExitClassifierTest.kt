package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ExitClassifierTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun findings(vararg lines: String, exit: String? = null, session: List<String> = emptyList()): LogSliceScanner.Findings {
        val f = tmp.newFile().apply {
            writeText((lines.toList() + listOfNotNull(
                exit?.let { "[2026-10-28 01:17:32] AppID 7 adding PID 5 as a tracked process \"x\"" },
                exit?.let { "[2026-10-28 01:20:00] AppID 7 no longer tracking PID 5, exit code $it" },
            )).joinToString("\n", postfix = "\n"))
        }
        val s = tmp.newFile().apply { writeText(session.joinToString("\n", postfix = "\n")) }
        return LogSliceScanner().scan(listOf(LogSliceScanner.Source("log", f), LogSliceScanner.Source("session.log", s, game = false)), appid = 7)
    }

    private fun ev(
        durS: Long = 600, f: LogSliceScanner.Findings? = findings(), frames: Long = 30_000,
        appKilled: ExitClassifier.AppExit? = null, stopped: Boolean = false, vortek: Int? = null, vortekToEnd: Long? = null,
        usedVortek: Boolean = false, zeroRun: Long = 0, stall: Double? = null, mean: Double? = 60.0, low1: Double? = 30.0,
        at60Thermal: Double? = 0.0, marked: Boolean = false, runMode: Boolean = false, status: Int? = null, fpsLimit: Int? = null,
        semPool: String? = null,
    ) = ExitClassifier.Evidence(
        durS = durS, appKilled = appKilled, sessionStopped = stopped, sessionStatus = status ?: if (stopped) 0 else null,
        vortekExit = vortek, vortekToEndS = vortekToEnd, usedVortek = usedVortek, findings = f, steamExitCode = f?.steamExitCode,
        gameFrames = frames, longestZeroRunS = zeroRun, longestStallS = stall, meanFpsAfterWarmup = mean, low1 = low1,
        at60Thermal = at60Thermal, marked = marked, runMode = runMode, fpsLimit = fpsLimit, semPool = semPool,
    )

    @Test fun reasonsInOrder() {
        val killed = ExitClassifier.AppExit("LOW_MEMORY", 0, "lmk", 1L)
        assertEquals("app_killed", ExitClassifier.classify(ev(appKilled = killed, f = findings("Crash!!!"))).reason)
        assertEquals("session_stopped", ExitClassifier.classify(ev(stopped = true)).reason)
        assertEquals("vortek_exit", ExitClassifier.classify(ev(vortek = 139, vortekToEnd = 3)).reason)
        assertEquals("vortek_exit", ExitClassifier.classify(ev(vortek = 139, vortekToEnd = 300, usedVortek = true)).reason)
        // The server died long before a hybris game ended: not why it ended.
        assertEquals("normal", ExitClassifier.classify(ev(vortek = 139, vortekToEnd = 300)).reason)
        assertEquals("device_lost", ExitClassifier.classify(ev(f = findings("err:   DxvkDevice: VK_ERROR_DEVICE_LOST"))).reason)
        assertEquals("normal", ExitClassifier.classify(ev()).reason)
    }

    @Test fun unityCrashWithExitCodeZeroIsACrash() {
        val exit = ExitClassifier.classify(ev(f = findings("Crash!!!", exit = "0")))
        assertEquals("crash", exit.reason)
        assertEquals(0, exit.steamCode)
        assertEquals("log: Crash!!!", exit.detail)
        assertTrue("crash" in exit.flags)
    }

    @Test fun nonZeroExitCodeIsACrashWithItsSignal() {
        val exit = ExitClassifier.classify(ev(f = findings(exit = "139")))
        assertEquals("crash", exit.reason)
        assertEquals(11, exit.signal)
        assertEquals("normal", ExitClassifier.classify(ev(f = findings(exit = "0"))).reason)
    }

    @Test fun shortPlaysWithoutAFrameOrWithMissingFeaturesFailedToLaunch() {
        assertEquals("launch_failed", ExitClassifier.classify(ev(durS = 20, frames = 0)).reason)
        assertEquals("normal", ExitClassifier.classify(ev(durS = 20, frames = 100)).reason)
        val skip = "err:   DxvkAdapter: Skipping: Device does not support required feature 'fillModeNonSolid'"
        val unsupported = ExitClassifier.classify(ev(frames = 0, f = findings(skip)))
        assertEquals("launch_failed", unsupported.reason)
        assertTrue(unsupported.detail!!.contains("fillModeNonSolid"))
        // DXVK skipping one adapter and drawing on another for two hours is not a failed launch.
        assertEquals("normal", ExitClassifier.classify(ev(frames = 400_000, f = findings(skip))).reason)
    }

    @Test fun aCrashBeforeTheFirstFrameOnMissingFeaturesIsAFailedLaunchWithTheCause() {
        // r9 2026-10-26-39: Duckov, 24 s, Player.log "Crash!!!" after DXVK found no adapter.
        val f = findings("err:   DxvkAdapter: Skipping: Device does not support required feature 'fillModeNonSolid'",
            "err:   DXVK: No adapters found", "Crash!!!")
        val exit = ExitClassifier.classify(ev(durS = 24, frames = 0, f = f))
        assertEquals("launch_failed", exit.reason)
        assertTrue(exit.detail!!, exit.detail!!.contains("fillModeNonSolid"))
        assertEquals(listOf("launch_failed", "crash"), exit.flags)
        // After frames, a crash names its cause before Unity's "Crash!!!".
        val later = ExitClassifier.classify(ev(f = findings("Scudo ERROR: out of memory", "Crash!!!")))
        assertEquals("crash", later.reason)
        assertEquals("log: Scudo ERROR: out of memory", later.detail)
    }

    @Test fun theSessionsLogsAloneDecideNoReason() {
        // Steam's web helper losing its device while the game played on.
        val exit = ExitClassifier.classify(ev(f = findings(session = listOf("VK_ERROR_DEVICE_LOST", "Crash!!!", "Out of memory"))))
        assertEquals("normal", exit.reason)
        assertEquals(emptyList<String>(), exit.flags)
    }

    @Test fun aGameRunDirectlyThatTheOwnerStoppedWasStopped() {
        assertEquals("session_stopped", ExitClassifier.classify(ev(runMode = true, stopped = true, status = 0)).reason)
        assertEquals("crash", ExitClassifier.classify(ev(runMode = true, status = 139)).reason)
        assertEquals("normal", ExitClassifier.classify(ev(runMode = true, status = 0)).reason)
    }

    @Test fun flagsAreIndependent() {
        val exit = ExitClassifier.classify(ev(stopped = true, zeroRun = 45, mean = 14.0, at60Thermal = 0.5, marked = true,
            f = findings("Scudo ERROR: out of memory", "hybris-x11: window 0x1 1704x1136 now hidden, frames paced (t=1)")))
        assertEquals("session_stopped", exit.reason)
        assertEquals(listOf("freeze", "low_fps", "oom", "hidden_paced", "at60_thermal", "marked"), exit.flags)
        // A long gap in the game layer's frame times alone (the X11 path took over for a while) is not a freeze.
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(stall = 40.0)).flags)
        assertEquals(listOf("low_fps"), ExitClassifier.classify(ev(low1 = 5.0)).flags)
        // Long frames in a game that draws on demand (a card table) are not stutter.
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(low1 = 5.0, mean = 12.0, fpsLimit = 15)).flags)
        assertEquals(listOf("low_fps"), ExitClassifier.classify(ev(mean = 12.0)).flags)
    }

    @Test fun aPackWithoutTheSemaphorePoolIsFlagged() {
        val said = "droiddeck-esync: pack valve-experimental-11.0-20261027b-arm64-1793301631-406ecaaf88ce-r1 for Proton Experimental " +
            "(ARM64) (1793301884 experimental-11.0-20261027b-arm64, exact match); droiddeck-ntsync off, droiddeck-esync on, " +
            "fds 33368/524288, no semaphore pool in this pack (every anonymous semaphore is a wineserver round trip)"
        val pooled = "droiddeck-esync: pack valve-r2 for Proton Experimental (ARM64) (1 v, exact match); droiddeck-ntsync off, " +
            "droiddeck-esync on, fds 33368/524288, semaphore pool on"
        assertEquals(listOf("no_sempool"), ExitClassifier.classify(ev(semPool = "none")).flags)
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(semPool = "on")).flags)
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(semPool = "off")).flags)
        // Without the game's environment the session's launch line decides.
        val f = findings(session = listOf(said))
        assertEquals(listOf("no_sempool"), ExitClassifier.classify(ev(f = f)).flags)
        assertTrue(f.firstLine("no_sempool")!!.startsWith("session.log: droiddeck-esync: pack valve-experimental-11.0-20261027b"))
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(f = f, semPool = "on")).flags)
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(f = findings(session = listOf(pooled)))).flags)
        assertEquals(null, ExitClassifier.keepFor(listOf("no_sempool")))
    }

    @Test fun thePreloadPoolStandingInForAPackIsNotFlagged() {
        val said = "droiddeck-esync: pack valve-r1 for Proton Experimental (ARM64) (1 v, exact match); droiddeck-ntsync off, " +
            "droiddeck-esync on, fds 33368/524288, preload semaphore pool on (this pack has none; create_semaphore 40, close_handle 21)"
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(semPool = "preload")).flags)
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(f = findings(session = listOf(said)))).flags)
    }

    @Test fun withoutTheEnvironmentOnlyLaunchLinesWithoutAnyPoolAreFlagged() {
        val pack = "droiddeck-esync: pack valve-r1 for Proton Experimental (ARM64) (1 v, exact match); droiddeck-ntsync off, " +
            "droiddeck-esync on, fds 33368/524288, "
        val kept = "droiddeck-esync: Wine is still running in this prefix; keeping its sync choice "
        val none = listOf(
            pack + "no semaphore pool in this pack (every anonymous semaphore is a wineserver round trip; BL_SEMPOOL_PRELOAD turns the " +
                "preload pool off)",
            "droiddeck-esync: no semaphore pool (BL_SEMPOOL_PRELOAD turns the preload pool off)",
            "droiddeck-esync: no semaphore pool (no request numbers for the preload pool in /p/Proton (ARM64)/files/bin-arm64/wineserver)",
            kept + "(droiddeck-esync, no semaphore pool)",
        )
        val pooled = listOf(
            "droiddeck-esync: preload semaphore pool on (create_semaphore 40, close_handle 21)",
            "droiddeck-esync: semaphore pool off (pack valve-r2 has one, but this launch runs the stock binaries)",
            "droiddeck-esync: semaphore pool off (pack valve-r2 has one, but BL_SYNC_NO_PACK keeps it out; BL_SEMPOOL_PRELOAD turns " +
                "the preload pool off)",
            kept + "(droiddeck-esync, semaphore pool off)",
            kept + "(droiddeck-ntsync, preload semaphore pool on)",
        )
        fun flags(line: String) = ExitClassifier.classify(ev(f = findings(session = listOf(line)))).flags
        for (line in none) assertEquals(line, listOf("no_sempool"), flags(line))
        for (line in pooled) assertEquals(line, emptyList<String>(), flags(line))
    }

    @Test fun halfAMinuteWithoutFramesWhileLoadingIsNotAFreeze() {
        // A.V.A.: its first frame, then 30 s of loading behind it.
        fun freeze(all: Long, afterLoad: Long) = "freeze" in ExitClassifier.classify(ExitClassifier.Evidence(
            durS = 600, gameFrames = 10_000, longestZeroRunS = all, zeroRunAfterLoadS = afterLoad, meanFpsAfterWarmup = 60.0)).flags
        assertTrue(!freeze(30, 0))
        assertTrue(freeze(95, 0))
        assertTrue(freeze(35, 35))
    }

    @Test fun keepDaysFollowTheFlags() {
        val day = 24L * 3600 * 1000
        assertEquals(null, ExitClassifier.keepFor(listOf("hidden_paced", "at60_thermal")))
        assertEquals(7 * day, ExitClassifier.keepFor(listOf("low_fps"))!!.first)
        assertEquals(14 * day, ExitClassifier.keepFor(listOf("low_fps", "crash"))!!.first)
        assertEquals(30 * day to listOf("crash", "marked"), ExitClassifier.keepFor(listOf("crash", "marked")))
    }
}
