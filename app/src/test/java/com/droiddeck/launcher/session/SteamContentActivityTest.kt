package com.droiddeck.launcher.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class SteamContentActivityTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun SteamContentActivity.feedAll(vararg lines: String) = apply { lines.forEach(::feed) }

    // content_log.txt, 2026-11-27: A.V.A. downloaded with Steamworks Shared, then its install script.
    private val download = arrayOf(
        "[2026-11-27 13:00:09] AppID 1473480 state changed : Update Required,Update Queued,Update Running,",
        "[2026-11-27 13:00:09] AppID 228980 state changed : Update Required,Fully Installed,Update Queued,Update Running,",
        "[2026-11-27 13:00:09] AppID 1473480 App update changed : Running Update,",
        "[2026-11-27 13:00:09] AppID 1473480 App update changed : Running Update,Reconfiguring,",
        "[2026-11-27 13:00:12] AppID 1473480 update started : download 0/13566412336, store 0/0, reuse 0/0, delta 0/0, stage 0/34532143628 ",
        "[2026-11-27 13:00:12] AppID 1473480 App update changed : Running Update,Downloading,Staging,",
        "[2026-11-27 13:02:24] Current download rate: 723.647 Mbps",
        "[2026-11-27 13:03:18] AppID 1473480 App update changed : Running Update,Committing,",
        "[2026-11-27 13:03:19] AppID 1473480 finished update, 1 mounted depots (BuildID 26722767) : 1473481 (3447061761948156248),",
    )
    private val script = arrayOf(
        "[2026-11-27 13:03:19] AppID 1473480 App update changed : Running Update,",
        "[2026-11-27 13:03:19] AppID 1473480 App update changed : Running Update,Running Script,",
    )
    private val done = arrayOf(
        "[2026-11-27 13:03:25] Current download rate: 0.000 Mbps",
        "[2026-11-27 13:03:25] AppID 1473480 App update changed : Running Update,",
        "[2026-11-27 13:03:25] AppID 1473480 App update changed : None",
        "[2026-11-27 13:03:25] AppID 228980 state changed : Fully Installed,Update Queued,",
        "[2026-11-27 13:03:25] AppID 1473480 state changed : Fully Installed,",
        "[2026-11-27 13:03:25] AppID 1473480 scheduler finished : removed from schedule (result No Error, state 0xc) ",
    )

    @Test fun onlyTheCommitAndTheInstallScriptOfADownloadKeepSteamBusy() {
        val activity = SteamContentActivity().feedAll(*download.copyOfRange(0, 7))
        assertEquals(emptyList<String>(), activity.busy())
        activity.feedAll(*download.copyOfRange(7, download.size))
        assertEquals(listOf("App 1473480 (Committing)"), activity.busy())
        activity.feed(script[0])
        assertEquals(emptyList<String>(), activity.busy())
        activity.feed(script[1])
        assertEquals(listOf("App 1473480 (Running Script)"), activity.busy())
        activity.feedAll(*done)
        assertEquals(emptyList<String>(), activity.busy())
    }

    @Test fun theNextQueuedDownloadIsNotBusyUntilItCommits() {
        val activity = SteamContentActivity().feedAll(*download, *script, *done,
            "[2026-11-27 13:03:26] AppID 730 App update changed : Running Update,",
            "[2026-11-27 13:03:26] AppID 730 App update changed : Running Update,Reconfiguring,",
            "[2026-11-27 13:03:26] AppID 730 App update changed : Running Update,Downloading,Staging,",
            "[2026-11-27 13:03:27] AppID 730 Shader update changed : Running Update,Downloading,",
            "[2026-11-27 13:03:27] AppID 4000 Workshop update changed : Running Update,Validating,",
        )
        assertEquals(emptyList<String>(), activity.busy())
        activity.feed("[2026-11-27 13:09:00] AppID 730 App update changed : Running Update,Committing,")
        assertEquals(listOf("App 730 (Committing)"), activity.busy())
    }

    @Test fun aSuspendedDownloadIsNotBusyAndAnUninstallIs() {
        val activity = SteamContentActivity().feedAll(
            "[2026-11-27 16:07:43] AppID 730 App update changed : Running Update,Committing,",
            "[2026-11-27 16:07:55] AppID 730 update canceled : Disabled (Suspended)",
            "[2026-11-27 16:07:55] AppID 730 App update changed : Running Update,Committing,Stopping,",
        )
        assertEquals(emptyList<String>(), activity.busy())
        activity.feedAll(
            "[2026-11-27 16:07:55] AppID 730 App update changed : None",
            "[2026-11-27 16:07:55] AppID 730 state changed : Update Required,Update Queued,Update Started, (Suspended)",
            "[2026-11-27 16:08:00] AppID 730 state changed : Update Required,Update Queued,Uninstalling,",
        )
        assertEquals(listOf("uninstall 730"), activity.busy())
        activity.feed("[2026-11-27 16:08:00] AppID 730 state changed : Uninstalled,")
        assertEquals(emptyList<String>(), activity.busy())
    }

    @Test fun shaderAndWorkshopCommitsCountAndAClientRestartForgetsEverything() {
        val activity = SteamContentActivity().feedAll(
            "[2026-11-27 13:26:59] AppID 1473480 Shader update changed : Running Update,Committing,",
            "[2026-11-27 13:27:00] AppID 4000 Workshop update changed : Running Update,Committing,",
            "[2026-11-27 13:27:00] AppID 730 state changed : Uninstalling,",
        )
        assertEquals(listOf("Shader 1473480 (Committing)", "Workshop 4000 (Committing)", "uninstall 730"), activity.busy())
        activity.feed("[2026-11-27 13:27:01] AppID 1473480 Shader update changed : None\r")
        assertEquals(listOf("Workshop 4000 (Committing)", "uninstall 730"), activity.busy())
        activity.feed("[2026-11-27 13:12:14] Client version: 1793740892")
        assertEquals(emptyList<String>(), activity.busy())
    }

    @Test fun pollReadsWhatWasAppendedAndOnlyWholeLines() {
        val log = tmp.newFile("content_log.txt")
        val activity = SteamContentActivity()
        assertEquals(emptyList<String>(), activity.poll(null))
        log.writeText(download.joinToString("\r\n", postfix = "\r\n"))
        assertEquals(listOf("App 1473480 (Committing)"), activity.poll(log))
        log.appendText(done.dropLast(1).joinToString("\r\n", postfix = "\r\n") + done.last().take(30))
        assertEquals(emptyList<String>(), activity.poll(log))
        log.appendText(done.last().drop(30) + "\r\n" + script.joinToString("\n", postfix = "\n") + "[2026-11-27 13:04:00] AppID 1 App update changed : Running Update,Running")
        assertEquals(listOf("App 1473480 (Running Script)"), activity.poll(log))
        log.appendText(" Script,\n")
        assertEquals(listOf("App 1473480 (Running Script)", "App 1 (Running Script)"), activity.poll(log))
    }

    @Test fun aNewLogStartsOverAndAMissingOneIsIdle() {
        val log = tmp.newFile("content_log.txt")
        val activity = SteamContentActivity()
        log.writeText(download.joinToString("\n", postfix = "\n"))
        assertEquals(listOf("App 1473480 (Committing)"), activity.poll(log))
        log.delete()
        assertEquals(emptyList<String>(), activity.poll(log))
        log.writeText(script.joinToString("\n", postfix = "\n"))
        assertEquals(listOf("App 1473480 (Running Script)"), activity.poll(log))
        File(tmp.root, "content_log.previous.txt").also { log.renameTo(it) }
        log.writeText("[2026-11-27 13:12:14] Client version: 1793740892\n")
        assertEquals(emptyList<String>(), activity.poll(log))
    }

    @Test fun aLongLogIsReadFromItsEnd() {
        val log = tmp.newFile("content_log.txt")
        val filler = (1..400).joinToString("\n", postfix = "\n") { "[2026-11-27 12:00:00] AppID 9 App update changed : Running Update,Committing," }
        log.writeText(filler + "[2026-11-27 12:10:00] AppID 9 App update changed : None\n" + download.joinToString("\n", postfix = "\n"))
        assertEquals(listOf("App 1473480 (Committing)"), SteamContentActivity(firstReadBytes = 2_000L).poll(log))
        log.writeText(download.joinToString("\n", postfix = "\n") + filler)
        val activity = SteamContentActivity(firstReadBytes = 2_000L)
        assertEquals(listOf("App 9 (Committing)"), activity.poll(log))
        log.appendText(done.joinToString("\n", postfix = "\n"))
        assertEquals(listOf("App 9 (Committing)"), activity.poll(log))
    }

    @Test fun theRunningClientsLogIsTheOneRead() {
        val root = tmp.newFolder("rootfs")
        assertNull(SteamContentActivity.contentLog(root, null))
        val arm = File(root, "root/.local/share/Steam/logs/content_log.txt").apply { parentFile.mkdirs(); writeText("") }
        val x64 = File(root, "root/.droiddeck-x64/.local/share/Steam/logs/content_log.txt").apply { parentFile.mkdirs(); writeText("") }
        arm.setLastModified(1_000_000L)
        x64.setLastModified(2_000_000L)
        assertEquals(arm, SteamContentActivity.contentLog(root, "arm64"))
        assertEquals(x64, SteamContentActivity.contentLog(root, "x86_64"))
        assertEquals(x64, SteamContentActivity.contentLog(root, null))
        assertTrue(arm.isFile)
        x64.writeText(script.joinToString("\n", postfix = "\n"))
        assertEquals(listOf("App 1473480 (Running Script)"), SteamContentActivity().poll(root, "x86_64"))
        assertEquals(emptyList<String>(), SteamContentActivity().poll(root, "arm64"))
        arm.writeText(download.joinToString("\n", postfix = "\n"))
        assertEquals(listOf("App 1473480 (Committing)"), SteamContentActivity().poll(root, "arm64"))
    }
}
