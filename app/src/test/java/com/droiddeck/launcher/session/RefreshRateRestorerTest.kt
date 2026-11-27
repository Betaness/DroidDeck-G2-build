package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.RefreshRateLiftLogic.Record
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

/** [RefreshRateLiftLogic.armCommand]'s restorer run by this machine's sh, with a stand-in for Android's settings command. */
class RefreshRateRestorerTest {
    private lateinit var dir: File
    private val watched = ArrayList<Process>()

    @Before fun setUp() {
        assumeTrue(File("/bin/sh").canExecute() && File("/proc/self").isDirectory)
        assumeTrue(System.getenv("PATH").orEmpty().split(':').any { File(it, "setsid").canExecute() })
        dir = Files.createTempDirectory("refresh-lift").toFile()
        File(dir, "settings").apply {
            writeText(
                """
                #!/bin/sh
                f="${'$'}(dirname "${'$'}0")/value"
                case "${'$'}1 ${'$'}2 ${'$'}3" in
                "get secure user_refresh_rate") if [ -f "${'$'}f" ]; then cat "${'$'}f"; else echo null; fi ;;
                "put secure user_refresh_rate") [ -f "${'$'}f.fail" ] && exit 1; echo "${'$'}4" > "${'$'}f" ;;
                "delete secure user_refresh_rate") rm -f "${'$'}f" ;;
                *) exit 1 ;;
                esac
                """.trimIndent() + "\n"
            )
            setExecutable(true)
        }
    }

    @After fun tearDown() {
        watched.forEach { it.destroy() }
        if (::dir.isInitialized) dir.deleteRecursively()
    }

    private val marker get() = File(dir, "marker")

    private var value: String?
        get() = File(dir, "value").takeIf(File::isFile)?.readText()?.trim()
        set(v) { if (v == null) File(dir, "value").delete() else File(dir, "value").writeText("$v\n") }

    private fun sh(line: String): Int {
        val builder = ProcessBuilder("/bin/sh", "-c", line).redirectErrorStream(true)
        builder.environment()["PATH"] = "${dir.path}:${System.getenv("PATH")}"
        return builder.start().waitFor()
    }

    /** A process standing in for the app's, and its restorer for [record]. */
    private fun armed(record: Record): Process {
        val app = ProcessBuilder("/bin/sh", "-c", "echo ${'$'}${'$'}; exec sleep 60").start().also { watched += it }
        val pid = app.inputStream.bufferedReader().readLine().trim().toInt()
        assertEquals(0, sh(RefreshRateLiftLogic.armCommand(marker.path, pid, record, pollSeconds = 1)))
        assertEquals(RefreshRateLiftLogic.markerLine(record), marker.readText().trim())
        return app
    }

    private fun gone(app: Process) {
        app.destroy()
        app.waitFor()
    }

    private fun waitFor(what: () -> Boolean) {
        val until = System.currentTimeMillis() + 10_000
        while (!what() && System.currentTimeMillis() < until) Thread.sleep(100)
    }

    @Test fun `once the process is gone the value before is put back`() {
        value = "144"
        val app = armed(Record("120", "144", "a1"))
        Thread.sleep(1_500)
        assertEquals("144", value)
        gone(app)
        waitFor { marker.readText().trim().endsWith("restored") }
        assertEquals("a1 120 144 restored", marker.readText().trim())
        assertEquals("120", value)
    }

    @Test fun `an unset value is deleted again`() {
        value = "144"
        val app = armed(Record(null, "144", "a2"))
        gone(app)
        waitFor { marker.readText().trim().endsWith("restored") }
        assertEquals("a2 unset 144 restored", marker.readText().trim())
        assertFalse(File(dir, "value").exists())
    }

    @Test fun `a value changed meanwhile is left`() {
        value = "144"
        val app = armed(Record("120", "144", "a3"))
        value = "90"
        gone(app)
        waitFor { marker.readText().trim().endsWith("left") }
        assertEquals("a3 120 144 left", marker.readText().trim())
        assertEquals("90", value)
    }

    @Test fun `put back by the app, the restorer stops and leaves a later 144 alone`() {
        value = "144"
        val app = armed(Record("120", "144", "a4"))
        assertEquals(0, sh(RefreshRateLiftLogic.putBackCommand(marker.path, "120")))
        assertEquals("120", value)
        assertFalse(marker.exists())
        value = "144"
        Thread.sleep(1_500)
        gone(app)
        Thread.sleep(2_500)
        assertEquals("144", value)
        assertFalse(marker.exists())
    }

    @Test fun `after a change the value written last is put back`() {
        value = "144"
        val app = armed(Record("120", "144", "c1"))
        assertEquals(0, sh(RefreshRateLiftLogic.changeCommand(marker.path, Record("120", "90", "c1", confirmed = false, previous = "144"))))
        assertEquals("90", value)
        assertEquals("c1 120 90", marker.readText().trim())
        Thread.sleep(1_500)
        assertEquals("90", value)
        gone(app)
        waitFor { marker.readText().trim().endsWith("restored") }
        assertEquals("c1 120 90 restored", marker.readText().trim())
        assertEquals("120", value)
    }

    @Test fun `a change whose write failed leaves the marker naming both values, and the one before is put back`() {
        value = "144"
        val app = armed(Record(null, "144", "c2"))
        File(dir, "value.fail").writeText("")
        assertFalse(sh(RefreshRateLiftLogic.changeCommand(marker.path, Record(null, "90", "c2", confirmed = false, previous = "144"))) == 0)
        File(dir, "value.fail").delete()
        assertEquals("144", value)
        assertEquals("c2 unset 90 144", marker.readText().trim())
        gone(app)
        waitFor { marker.readText().trim().endsWith("restored") }
        assertEquals("c2 unset 90 restored", marker.readText().trim())
        assertFalse(File(dir, "value").exists())
    }

    @Test fun `during a change either value is put back and any other is left`() {
        for ((now, outcome) in listOf("90" to "restored", "144" to "restored", "60" to "left")) {
            value = "144"
            val id = "d$now"
            val app = armed(Record("120", "144", id))
            marker.writeText("$id 120 90 144
")
            value = now
            gone(app)
            waitFor { marker.readText().trim().endsWith(outcome) }
            assertEquals("$id 120 90 $outcome", marker.readText().trim())
            assertEquals(if (outcome == "restored") "120" else now, value)
        }
    }

    @Test fun `a later restorer replaces an earlier one`() {
        value = "144"
        val first = armed(Record("120", "144", "a5"))
        val second = armed(Record("60", "144", "a6"))
        gone(first)
        Thread.sleep(2_500)
        assertEquals("144", value)
        assertEquals("a6 60 144", marker.readText().trim())
        gone(second)
        waitFor { marker.readText().trim().endsWith("restored") }
        assertEquals("a6 60 144 restored", marker.readText().trim())
        assertEquals("60", value)
    }
}
