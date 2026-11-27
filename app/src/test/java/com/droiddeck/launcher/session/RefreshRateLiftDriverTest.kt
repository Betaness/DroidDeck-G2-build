package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.RefreshRateLiftDriver.Rates
import com.droiddeck.launcher.session.RefreshRateLiftDriver.Want
import com.droiddeck.launcher.session.RefreshRateLiftLogic.Record
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class RefreshRateLiftDriverTest {
    private var now = 0L

    /** The scheduler's thread: tasks run in time order, then in the order given, on [runFor]. */
    private inner class Tasks : RefreshRateLiftDriver.Scheduler {
        private inner class Task(val at: Long, val order: Int, val block: () -> Unit) {
            var cancelled = false
        }

        private val queue = ArrayList<Task>()
        private var order = 0

        override fun execute(task: () -> Unit) {
            queue += Task(now, order++, task)
        }

        override fun schedule(delayMs: Long, task: () -> Unit): RefreshRateLiftDriver.Cancel {
            val t = Task(now + delayMs, order++, task)
            queue += t
            return RefreshRateLiftDriver.Cancel { t.cancelled = true }
        }

        /** How long each task still waits. */
        fun waiting() = queue.filter { !it.cancelled }.map { it.at - now }.sorted()

        fun runFor(ms: Long = 0) {
            val end = now + ms
            while (true) {
                val next = queue.filter { !it.cancelled && it.at <= end }.minWithOrNull(compareBy<Task>({ it.at }, { it.order })) ?: break
                queue.remove(next)
                now = maxOf(now, next.at)
                next.block()
            }
            queue.removeAll { it.cancelled }
            now = end
        }
    }

    private class Secure(var value: String?) : RefreshRateLiftLogic.Secure {
        var failing = false
        val writes = ArrayList<String?>()
        var marker: String? = null

        override fun read(): String? {
            check(!failing) { "no pairing" }
            return value
        }

        override fun put(value: String) {
            check(!failing) { "no pairing" }
            writes += value
            this.value = value
        }

        override fun putBack(original: String?) {
            check(!failing) { "no pairing" }
            writes += original
            value = original
            marker = null
        }

        override fun marker(): RefreshRateLiftLogic.Marker? {
            check(!failing) { "no pairing" }
            return RefreshRateLiftLogic.parseMarker(marker ?: "")
        }

        override fun arm(record: Record) {
            check(!failing) { "no pairing" }
            marker = RefreshRateLiftLogic.markerLine(record)
        }

        override fun change(record: Record) {
            check(!failing) { "no pairing" }
            writes += record.written
            value = record.written
            marker = RefreshRateLiftLogic.markerLine(record.copy(previous = null))
        }

        override fun disarm() {
            check(!failing) { "no pairing" }
            marker = null
        }
    }

    private class Records : RefreshRateLiftLogic.Records {
        var record: Record? = null
        override fun load() = record
        override fun save(record: Record) {
            this.record = record
        }
        override fun clear() {
            record = null
        }
    }

    private class Events : RefreshRateLiftDriver.Events {
        val lines = ArrayList<String>()
        val failures = ArrayList<Throwable>()
        var callsDone = 0
        var watching = false

        override fun line(text: String) {
            lines += text
        }

        override fun failed(t: Throwable) {
            failures += t
        }

        override fun callDone() {
            callsDone++
        }

        override fun watchDebugging(on: Boolean) {
            watching = on
        }
    }

    private val tasks = Tasks()
    private val secure = Secure("120")
    private val records = Records()
    private val events = Events()
    private var enabled = true
    private var modes = listOf(60f, 90f, 120f, 144f)
    private var ids = 0
    private var logics = 0
    private val driver = RefreshRateLiftDriver(tasks, { enabled }, { modes }, events) {
        logics++
        RefreshRateLiftLogic(secure, records, { now }) { "r${++ids}" }
    }

    @Test fun `the top rate and the mode for the rate wanted are the panel's`() {
        assertEquals(Rates(top = 144, hz = 90), RefreshRateLiftDriver.rates(89.9f, modes))
        assertEquals(Rates(top = 144, hz = 144), RefreshRateLiftDriver.rates(0f, modes))
        assertEquals(Rates(top = 120, hz = 60), RefreshRateLiftDriver.rates(45f, listOf(60f, 120f)))
        assertEquals(Rates(top = 0, hz = 0), RefreshRateLiftDriver.rates(90f, emptyList()))
    }

    @Test fun `a rate wanted before the session is shown is written as the panel's mode for it`() {
        driver.want(45f)
        driver.shown()
        tasks.runFor()
        assertEquals("60", secure.value)
        assertEquals(Record("120", "60", "r1"), records.record)
        assertEquals(1, logics)
        assertTrue(tasks.waiting().isEmpty())
    }

    @Test fun `a rate wanted again within a hundredth is dropped, and one wanted later is written when due`() {
        driver.shown()
        tasks.runFor()
        assertEquals("144", secure.value)
        driver.want(89.9f)
        driver.want(89.905f)
        assertEquals(89.9f, driver.wantedHz, 0f)
        assertEquals(listOf(0L), tasks.waiting())
        tasks.runFor()
        assertEquals(listOf(RefreshRateLiftLogic.DWELL_MS), tasks.waiting())
        tasks.runFor(RefreshRateLiftLogic.DWELL_MS - 1)
        assertEquals("144", secure.value)
        tasks.runFor(1)
        assertEquals("90", secure.value)
        assertTrue(tasks.waiting().isEmpty())

        driver.want(0.5f)
        assertEquals(0f, driver.wantedHz, 0f)
        tasks.runFor()
        assertEquals(listOf(RefreshRateLiftLogic.DWELL_MS), tasks.waiting())
        tasks.runFor(RefreshRateLiftLogic.DWELL_MS)
        assertEquals("144", secure.value)
        assertEquals(listOf<String?>("144", "90", "144"), secure.writes)
        assertTrue(tasks.waiting().isEmpty())
    }

    @Test fun `hiding drops a change that waits, and showing again writes the rate wanted at once`() {
        driver.shown()
        tasks.runFor()
        driver.want(90f)
        tasks.runFor()
        assertEquals(listOf(RefreshRateLiftLogic.DWELL_MS), tasks.waiting())
        driver.hidden(grace = true)
        tasks.runFor(RefreshRateLiftDriver.HIDE_GRACE_MS - 1)
        assertEquals("144", secure.value)
        tasks.runFor(1)
        assertEquals("120", secure.value)
        assertTrue(tasks.waiting().isEmpty())
        tasks.runFor(60_000)
        assertEquals(listOf<String?>("144", "120"), secure.writes)

        driver.shown()
        tasks.runFor()
        assertEquals("90", secure.value)
        assertTrue(tasks.waiting().isEmpty())
        driver.hidden(grace = true)
        tasks.runFor(1_000)
        driver.shown()
        tasks.runFor(60_000)
        assertEquals("90", secure.value)
        driver.hidden(grace = false)
        tasks.runFor()
        assertEquals("120", secure.value)
        assertEquals(listOf<String?>("144", "120", "90", "120"), secure.writes)
        assertEquals(1, logics)
        assertFalse(events.watching)
    }

    @Test fun `with the setting off nothing is planned`() {
        driver.shown()
        tasks.runFor()
        enabled = false
        driver.want(90f)
        tasks.runFor()
        assertTrue(tasks.waiting().isEmpty())
        assertEquals("144", secure.value)
    }

    @Test fun `a change that failed while shown is tried again, sooner when wireless debugging comes on`() {
        driver.shown()
        tasks.runFor()
        driver.want(90f)
        tasks.runFor(RefreshRateLiftLogic.DWELL_MS)
        assertEquals("90", secure.value)
        driver.want(0f)
        tasks.runFor()
        secure.failing = true
        tasks.runFor(RefreshRateLiftLogic.DWELL_MS)
        assertEquals("90", secure.value)
        assertEquals(1, events.failures.size)
        assertTrue(events.watching)
        assertEquals(listOf(RefreshRateLiftDriver.RETRY_MS[0]), tasks.waiting())
        assertEquals("user_refresh_rate not set to the rate the session wants (no pairing): trying again in 15 s, " +
            "then less often, and when wireless debugging comes on", events.lines.last())

        tasks.runFor(RefreshRateLiftDriver.RETRY_MS[0])
        assertEquals(listOf(RefreshRateLiftDriver.RETRY_MS[1]), tasks.waiting())
        assertEquals(1, events.failures.size)

        secure.failing = false
        driver.debuggingCameOn()
        tasks.runFor(RefreshRateLiftDriver.WIRELESS_DEBUGGING_ON_MS)
        assertEquals("144", secure.value)
        assertEquals("user_refresh_rate 90 -> 144, the rate the session wants now", events.lines.last())
        assertFalse(events.watching)
        assertTrue(tasks.waiting().isEmpty())
        assertEquals(listOf<String?>("144", "90", "144"), secure.writes)
        assertEquals(5, events.callsDone)
    }

    @Test fun `a first write that failed is not tried again while shown`() {
        secure.failing = true
        driver.shown()
        tasks.runFor()
        assertEquals(1, events.failures.size)
        assertFalse(events.watching)
        assertTrue(tasks.waiting().isEmpty())
        assertNull(records.record)
    }

    @Test fun `a value that could not be put back is tried again`() {
        driver.shown()
        tasks.runFor()
        secure.failing = true
        driver.hidden(grace = false)
        tasks.runFor()
        assertEquals("user_refresh_rate not put back (no pairing): trying again in 15 s, " +
            "then less often, and when wireless debugging comes on", events.lines.last())
        secure.failing = false
        tasks.runFor(RefreshRateLiftDriver.RETRY_MS[0])
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertTrue(tasks.waiting().isEmpty())
    }

    @Test fun `the device shutting down puts the value back with a session on screen`() {
        driver.shown()
        tasks.runFor()
        driver.want(90f)
        tasks.runFor()
        var done = 0
        driver.shutdown { done++ }
        assertEquals(0, done)
        tasks.runFor()
        assertEquals(1, done)
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)
        tasks.runFor(60_000)
        assertEquals(listOf<String?>("144", "120"), secure.writes)

        // A grace hide that waits is dropped; with nothing raised the shutdown needs no shell.
        driver.hidden(grace = true)
        secure.failing = true
        driver.shutdown { done++ }
        tasks.runFor(60_000)
        assertEquals(2, done)
        assertTrue(events.failures.isEmpty())
    }

    @Test fun `a shutdown that fails still tells it is done, and the value is tried again`() {
        driver.shown()
        tasks.runFor()
        secure.failing = true
        var done = 0
        driver.shutdown { done++ }
        tasks.runFor()
        assertEquals(1, done)
        assertEquals("144", secure.value)
        assertEquals(listOf(RefreshRateLiftDriver.RETRY_MS[0]), tasks.waiting())
        secure.failing = false
        tasks.runFor(RefreshRateLiftDriver.RETRY_MS[0])
        assertEquals("120", secure.value)
    }

    @Test fun `the session wants ZeroFG's vote on this screen, else the limit's mode, else the top rate`() {
        var reads = 0
        val vote = {
            reads++
            90f
        }
        fun want(zeroFg: Boolean = true, bottom: Float = 0f, glasses: Float = 0f, limit: Int = 0, mirror: Int = 0) =
            RefreshRateLiftDriver.sessionWant(zeroFg, vote, bottom, glasses, limit, mirror) { 120f }

        assertEquals(Want(90f, "ZeroFG's frame-rate vote"), want(limit = 60, mirror = 72))
        assertEquals(1, reads)
        assertEquals(Want(120f, "the mode that divides 60 fps"), want(zeroFg = false, limit = 60))
        assertEquals(Want(120f, "the mode that divides 60 fps"), want(bottom = 60f, limit = 60))
        assertEquals(Want(0f, "the panel's top rate"), want(glasses = 90f))
        assertEquals(1, reads)
        assertEquals(Want(90f, "ZeroFG's frame-rate vote"), want(bottom = 1f, glasses = 0.5f))
        assertEquals(Want(120f, "the mirror's rate"), want(zeroFg = false, mirror = 72))
        assertEquals(Want(0f, "the panel's top rate"), want(zeroFg = false))

        assertEquals(Want(0f, "the panel's top rate"), RefreshRateLiftDriver.sessionWant(true, { 1f }, 0f, 0f, 0, 0) { 120f })
        assertEquals(Want(120f, "the mode that divides 30 fps"),
            RefreshRateLiftDriver.sessionWant(true, { throw UnsatisfiedLinkError("no compositor") }, 0f, 0f, 30, 0) { 120f })
        assertEquals(Want(0f, "the mode that divides 30 fps"),
            RefreshRateLiftDriver.sessionWant(false, vote, 0f, 0f, 30, 0) { error("no display") })
    }
}
