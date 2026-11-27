package com.droiddeck.launcher.session

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class SteamGameProcessTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun cmdline(vararg args: String) = args.joinToString("\u0000", postfix = "\u0000").toByteArray()

    @Test fun theSessionsOwnSearchesAreNotAGame() {
        assertFalse(SteamGameProcess.matches(cmdline("pgrep", "-a", "-f", "SteamLaunch AppId=[0-9]")))
        assertFalse(SteamGameProcess.matches(cmdline("pgrep", "-f", "reaper SteamLaunch")))
        assertFalse(SteamGameProcess.matches(cmdline("pgrep", "-f", "reaper SteamLaunch AppId=976310( |$)")))
        assertFalse(SteamGameProcess.matches(cmdline("sh", "-c", "pgrep -f 'proton waitforexitandrun'")))
        assertFalse(SteamGameProcess.matches(cmdline("/usr/bin/steam", "steam://rungameid/976310")))
    }

    @Test fun steamsReaperAndProtonAreAGame() {
        assertTrue(SteamGameProcess.matches(cmdline("/root/.local/share/Steam/steamrtarm64/reaper", "SteamLaunch", "AppId=976310", "--",
            "/root/.local/share/Steam/compatibilitytools.d/droiddeck-proton-arm64/proton", "waitforexitandrun", "MK11.exe")))
        assertTrue(SteamGameProcess.matches(cmdline("python3", "/proton", "waitforexitandrun", "game.exe")))
    }

    // The process trees Steam started on 2026-11-27: A.V.A.'s install script after its download, and a game.
    private val steam = "/root/.local/share/Steam"
    private val tool = "$steam/compatibilitytools.d/droiddeck-proton-arm64/droiddeck-proton"
    private val installScript = listOf(
        arrayOf("/bin/sh", "-c", "$steam/linuxarm64/steam-launch-wrapper --oom-score-adjust 900 -- $steam/steamrtarm64/reaper SteamLaunch " +
            "AppId=1473480 Install=1 -- '$steam/compatibilitytools.d/droiddeck-proton-arm64'/droiddeck-proton run " +
            "'$steam/legacycompat/iscriptevaluator.exe' 'legacycompat\\evaluatorscript_1473480.vdf'"),
        arrayOf("$steam/linuxarm64/pw-audio-namespace", "--", "$steam/steamrtarm64/reaper", "SteamLaunch", "AppId=1473480", "Install=1", "--",
            tool, "run", "$steam/legacycompat/iscriptevaluator.exe", "legacycompat\\evaluatorscript_1473480.vdf"),
        arrayOf("$steam/steamrtarm64/reaper", "SteamLaunch", "AppId=1473480", "Install=1", "--",
            tool, "run", "$steam/legacycompat/iscriptevaluator.exe", "legacycompat\\evaluatorscript_1473480.vdf"),
        arrayOf("python3", "/root/.local/share/droiddeck-esync/dist/valve-experimental-11.0/proton", "run",
            "$steam/legacycompat/iscriptevaluator.exe", "legacycompat\\evaluatorscript_1473480.vdf"),
        arrayOf("c:\\windows\\system32\\steam.exe", "$steam/legacycompat/iscriptevaluator.exe", "legacycompat\\evaluatorscript_1473480.vdf"),
        arrayOf("wineserver"),
        arrayOf("C:\\windows\\system32\\services.exe"),
        arrayOf("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Steamworks Shared\\_CommonRedist\\vcredist\\2022\\VC_redist.x64.exe", "/q"),
        arrayOf("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Steamworks Shared\\_CommonRedist\\DirectX\\Jun2010\\DXSETUP.exe", "/silent"),
    )
    private val game = listOf(
        arrayOf("/bin/sh", "-c", "$steam/linuxarm64/steam-launch-wrapper --oom-score-adjust 900 -- $steam/steamrtarm64/reaper SteamLaunch " +
            "AppId=3167020 -- '$steam/compatibilitytools.d/droiddeck-proton-arm64'/droiddeck-proton waitforexitandrun  " +
            "'$steam/steamapps/common/Escape from Duckov/Duckov.exe'"),
        arrayOf("$steam/linuxarm64/pw-audio-namespace", "--", "$steam/steamrtarm64/reaper", "SteamLaunch", "AppId=3167020", "--",
            tool, "waitforexitandrun", "$steam/steamapps/common/Escape from Duckov/Duckov.exe"),
        arrayOf("$steam/steamrtarm64/reaper", "SteamLaunch", "AppId=3167020", "--", tool, "waitforexitandrun",
            "$steam/steamapps/common/Escape from Duckov/Duckov.exe"),
        arrayOf("python3", "/root/.local/share/droiddeck-esync/dist/valve-experimental-11.0/proton", "waitforexitandrun",
            "$steam/steamapps/common/Escape from Duckov/Duckov.exe", "-job-worker-count", "3"),
        arrayOf("c:\\windows\\system32\\steam.exe", "$steam/steamapps/common/Escape from Duckov/Duckov.exe", "-job-worker-count", "3"),
        arrayOf("S:\\steamapps\\common\\Escape from Duckov\\Duckov.exe", "-job-worker-count", "3"),
    )

    @Test fun anInstallScriptAndTheInstallersItRunsAreNotAGame() {
        installScript.forEach { assertFalse(it.joinToString(" "), SteamGameProcess.matches(cmdline(*it))) }
        val proc = tmp.newFolder("proc")
        installScript.forEachIndexed { i, args -> File(proc, "${300 + i}").apply { mkdirs() }.resolve("cmdline").writeBytes(cmdline(*args)) }
        assertFalse(SteamGameProcess.running(proc))
    }

    @Test fun theGamesReaperAndProtonAreAGame() {
        assertFalse(SteamGameProcess.matches(cmdline(*game[0])))
        game.subList(1, 4).forEach { assertTrue(it.joinToString(" "), SteamGameProcess.matches(cmdline(*it))) }
        assertFalse(SteamGameProcess.matches(cmdline(*game[4])))
        assertFalse(SteamGameProcess.matches(cmdline(*game[5])))
    }

    @Test fun otherToolVerbsAreNotAGame() {
        assertFalse(SteamGameProcess.matches(cmdline("reaper", "SteamLaunch", "AppId=220", "--", tool, "getcompatpath", "/x")))
        assertTrue(SteamGameProcess.matches(cmdline("reaper", "SteamLaunch", "AppId=70", "--", "$steam/steamapps/common/Half-Life/hl_linux", "-steam")))
    }

    @Test fun runningLooksAtEveryProcess() {
        val proc = tmp.newFolder("proc")
        fun process(pid: String, vararg args: String) = File(proc, pid).apply { mkdirs() }.resolve("cmdline").writeBytes(cmdline(*args))
        process("100", "pgrep", "-a", "-f", "SteamLaunch AppId=[0-9]")
        process("self", "reaper", "SteamLaunch", "AppId=1")
        assertFalse(SteamGameProcess.running(proc))
        process("200", "reaper", "SteamLaunch", "AppId=976310", "--", "proton", "waitforexitandrun", "MK11.exe")
        assertTrue(SteamGameProcess.running(proc))
    }
}
