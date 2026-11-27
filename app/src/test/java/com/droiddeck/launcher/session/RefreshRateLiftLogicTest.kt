package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.RefreshRateLiftLogic.Record
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RefreshRateLiftLogicTest {
    /** The setting and the marker through the pairing; [processDied] does what the restorer does. */
    private class FakeSecure(var value: String?) : RefreshRateLiftLogic.Secure {
        var reads = 0
        val writes = ArrayList<String?>()
        var failing = false
        var failingPut = false
        /** A change's write does not go through; its first marker line is written. */
        var failingChange = false
        var marker: String? = null
        /** The restorers started, by id; the last one runs, an earlier one stops at its next look. */
        val armed = ArrayList<String>()

        override fun read(): String? {
            check(!failing) { "no pairing" }
            reads++
            return value
        }

        override fun put(value: String) {
            check(!failing && !failingPut) { "no pairing" }
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
            armed += record.id
        }

        override fun change(record: Record) {
            check(!failing) { "no pairing" }
            marker = RefreshRateLiftLogic.markerLine(record)
            check(!failingChange) { "settings put failed" }
            writes += record.written
            value = record.written
            marker = RefreshRateLiftLogic.markerLine(record.copy(previous = null))
        }

        override fun disarm() {
            check(!failing) { "no pairing" }
            marker = null
        }

        /** The app's process is gone: its restorer, when its marker still names it, acts as [RefreshRateLiftLogic.armCommand]'s does. */
        fun processDied() {
            val running = armed.lastOrNull() ?: return
            armed.clear()
            val m = RefreshRateLiftLogic.parseMarker(marker ?: "") ?: return
            if (m.id != running || m.outcome != null) return
            val line = "${m.id} ${m.original ?: "unset"} ${m.written}"
            marker = if (value == m.written || (m.previous != null && value == m.previous)) {
                value = m.original
                writes += m.original
                "$line ${RefreshRateLiftLogic.RESTORED}"
            } else "$line ${RefreshRateLiftLogic.LEFT}"
        }
    }

    private class FakeRecords : RefreshRateLiftLogic.Records {
        var record: Record? = null
        override fun load() = record
        override fun save(record: Record) { this.record = record }
        override fun clear() { record = null }
    }

    private val records = FakeRecords()
    private var ids = 0
    private var now = 0L

    private fun logic(secure: FakeSecure) = RefreshRateLiftLogic(secure, records, { now }) { "r${++ids}" }

    @Test fun `raised while shown and put back after, with the value before recorded and a restorer started first`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 120 -> 144 while a session is on screen", lift.shown(true, 144))
        assertEquals("144", secure.value)
        assertEquals(Record("120", "144", "r1", confirmed = true), records.record)
        assertEquals("r1 120 144", secure.marker)
        assertEquals(listOf("r1"), secure.armed)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)
        assertEquals(listOf<String?>("144", "120"), secure.writes)
    }

    @Test fun `a value the owner changed meanwhile is left, and the record and the restorer dropped`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        secure.value = "90"
        assertEquals("user_refresh_rate 90, changed from 144 meanwhile: left as it is", lift.hidden())
        assertEquals("90", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)
        assertEquals(listOf<String?>("144"), secure.writes)
    }

    @Test fun `a raised value left by a process that died is put back at the next app or session start`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.armed.clear() // the restorer did not run (the device restarted)

        val afterCrash = logic(secure)
        assertEquals("user_refresh_rate 144 -> 120 again", afterCrash.appStarted())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)

        logic(secure).shown(true, 144)
        secure.armed.clear()
        assertEquals("user_refresh_rate 144 -> 120 again", logic(secure).sessionStarted(true, 144))
        assertEquals("120", secure.value)
        assertNull(records.record)
    }

    @Test fun `the restorer puts the value back when the process dies, and the next process only drops its record`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.processDied()
        assertEquals("120", secure.value)
        assertEquals("r1 120 144 restored", secure.marker)

        assertEquals("user_refresh_rate 120, changed from 144 meanwhile: left as it is", logic(secure).appStarted())
        assertNull(records.record)
        assertNull(secure.marker)
        assertEquals(listOf<String?>("144", "120"), secure.writes)
    }

    @Test fun `the owner's own 144 after the restorer acted is not overwritten`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.processDied()
        secure.value = "144"

        val next = logic(secure)
        assertEquals("user_refresh_rate 144, put back by the restorer and set to 144 again since: left as it is", next.appStarted())
        assertEquals("144", secure.value)
        assertNull(records.record)

        // At a session instead of an app start, after the restorer found it changed: nothing raised, nothing recorded.
        records.record = Record("120", "144", "r2")
        secure.marker = "r2 120 144 left"
        assertEquals("user_refresh_rate 144, changed and set to 144 again since: already reaches the panel's 144 Hz", logic(secure).shown(true, 144))
        assertNull(records.record)
        assertEquals("144", secure.value)
    }

    @Test fun `a record whose write was never confirmed by a process that died is dropped without a write`() {
        val secure = FakeSecure("144")
        records.record = Record("120", "144", "r9", confirmed = false)
        assertEquals("user_refresh_rate 144, the write of 144 was not confirmed: left as it is", logic(secure).appStarted())
        assertEquals("144", secure.value)
        assertNull(records.record)
        assertTrue(secure.writes.isEmpty())
    }

    @Test fun `a write that failed in this process is still put back`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        secure.failingPut = true
        assertThrows(IllegalStateException::class.java) { lift.shown(true, 144) }
        assertEquals(Record("120", "144", "r1", confirmed = false), records.record)
        assertEquals("r1 120 144", secure.marker)
        // The write went through after all (the answer was lost).
        secure.value = "144"
        secure.failingPut = false
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
        assertNull(records.record)
    }

    @Test fun `a record lost with the app's data is taken from the restorer's marker`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.armed.clear() // the restorer did not run (the device restarted)
        records.record = null // the app's data cleared

        val lift = logic(secure)
        assertEquals("user_refresh_rate 144 recorded only in the restorer's marker: kept, the restorer now watches this process", lift.shown(true, 144))
        assertEquals(Record("120", "144", "r2"), records.record)
        assertEquals("r2 120 144", secure.marker)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
    }

    @Test fun `a new process keeps a value raised before and starts its own restorer`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.armed.clear()
        val lift = logic(secure)
        assertEquals("user_refresh_rate 144 raised before: kept, the restorer now watches this process", lift.shown(true, 144))
        assertEquals(Record("120", "144", "r2"), records.record)
        assertEquals("r2 120 144", secure.marker)
        assertNull(lift.shown(true, 144))
        assertEquals(listOf<String?>("144"), secure.writes)
    }

    @Test fun `nothing is put back at app or session start while a session activity is shown`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        assertNull(lift.appStarted())
        assertNull(lift.sessionStarted(true, 144))
        assertEquals("144", secure.value)
        assertEquals(Record("120", "144", "r1"), records.record)
        assertNull(lift.retry(true))
        assertFalse(lift.pending(true))
        assertTrue(lift.pending(false))
    }

    @Test fun `an unset value is recorded as unset and deleted again`() {
        val secure = FakeSecure(null)
        val lift = logic(secure)
        assertEquals("user_refresh_rate unset -> 144 while a session is on screen", lift.shown(true, 144))
        assertEquals(Record(null, "144", "r1"), records.record)
        assertEquals("r1 unset 144", secure.marker)
        assertEquals("user_refresh_rate 144 -> unset again", lift.hidden())
        assertNull(secure.value)
        assertNull(records.record)
        assertEquals(listOf<String?>("144", null), secure.writes)
    }

    @Test fun `turning the setting off puts the value back at once`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.turnedOff())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertTrue(lift.visible)
    }

    @Test fun `with the setting off nothing is read or written`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        assertNull(lift.shown(false, 144))
        assertNull(lift.hidden())
        assertNull(lift.turnedOff())
        assertEquals(0, secure.reads)
        assertTrue(secure.writes.isEmpty())
        assertTrue(secure.armed.isEmpty())
    }

    @Test fun `a value that reaches the top rate already is left alone`() {
        val secure = FakeSecure("144")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 144: already reaches the panel's 144 Hz", lift.shown(true, 144))
        assertNull(records.record)
        assertNull(lift.hidden())
        assertEquals("144", secure.value)
        assertTrue(secure.writes.isEmpty())
        assertTrue(secure.armed.isEmpty())
        assertFalse(RefreshRateLiftLogic.below("144.0", 144))
        assertFalse(RefreshRateLiftLogic.below("165", 144))
        assertFalse(RefreshRateLiftLogic.below("120", 0))
    }

    @Test fun `repeated starts and stops keep the first value`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        repeat(3) {
            lift.shown(true, 144)
            val reads = secure.reads
            assertNull(lift.shown(true, 144))
            assertNull(lift.sessionStarted(true, 144))
            assertEquals(reads + 2, secure.reads)
            assertEquals("120", records.record?.original)
            assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
            assertNull(lift.hidden())
            assertNull(lift.sessionEnded())
            assertEquals("120", secure.value)
        }
        assertEquals(listOf<String?>("144", "120", "144", "120", "144", "120"), secure.writes)
        assertEquals(listOf("r1", "r2", "r3"), secure.armed)
    }

    @Test fun `the session end puts it back, and a new session in a shown activity raises it again`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.sessionEnded())
        assertEquals("user_refresh_rate 120 -> 144 while a session is on screen", lift.sessionStarted(true, 144))
        assertEquals("144", secure.value)
    }

    @Test fun `a value changed by the owner while shown starts a new record`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        secure.value = "60"
        assertEquals("user_refresh_rate 60, changed from 144 meanwhile -> 144 while a session is on screen", lift.shown(true, 144))
        assertEquals(Record("60", "144", "r2"), records.record)
        lift.hidden()
        assertEquals("60", secure.value)

        lift.shown(true, 144)
        secure.value = "144.0"
        assertNull(lift.shown(true, 144))
        secure.value = "165"
        assertEquals("user_refresh_rate 165, changed from 144 meanwhile: already reaches the panel's 144 Hz", lift.shown(true, 144))
        assertNull(records.record)
        assertNull(secure.marker)
    }

    @Test fun `a failing pairing changes nothing, and a record left is put back by a retry`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        secure.failing = true
        assertThrows(IllegalStateException::class.java) { lift.shown(true, 144) }
        assertNull(records.record)
        assertEquals("120", secure.value)
        assertFalse(lift.pending(true))

        secure.failing = false
        lift.shown(true, 144)
        secure.failing = true
        assertThrows(IllegalStateException::class.java) { lift.hidden() }
        assertEquals(Record("120", "144", "r1"), records.record)
        assertFalse(lift.visible)
        assertTrue(lift.pending(true))
        assertThrows(IllegalStateException::class.java) { lift.retry(true) }
        secure.failing = false
        assertEquals("user_refresh_rate 144 -> 120 again", lift.retry(true))
        assertEquals("120", secure.value)
        assertFalse(lift.pending(true))
    }

    @Test fun `a restorer that could not be started leaves the setting and no record`() {
        val secure = object : RefreshRateLiftLogic.Secure by FakeSecure("120") {
            override fun arm(record: Record) = throw IllegalStateException("/data/local/tmp not writable")
        }
        assertThrows(IllegalStateException::class.java) { RefreshRateLiftLogic(secure, records) { "r1" }.shown(true, 144) }
        assertEquals("120", secure.read())
        assertNull(records.record)
    }

    @Test fun `a record that is not a rate is dropped without a write`() {
        val secure = FakeSecure("144")
        records.record = Record("120; reboot", "144", "r1")
        assertEquals("record of user_refresh_rate not understood (120; reboot -> 144): dropped", logic(secure).appStarted())
        assertNull(records.record)
        assertTrue(secure.writes.isEmpty())
        records.record = Record("120", "144", "r1; reboot")
        logic(secure).appStarted()
        assertNull(records.record)
        assertTrue(secure.writes.isEmpty())
    }

    @Test fun `another rate wanted is written once it held for the steady time and the last write is the dwell old`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 120 -> 144 while a session is on screen", lift.shown(true, 144))
        lift.want(90, 144)
        assertEquals(5_000L, lift.due(true))
        now = 1_000
        lift.want(120, 144)
        now = 1_500
        lift.want(90, 144)
        assertEquals(3_500L, lift.due(true))
        now = 4_999
        assertNull(lift.settle(true))
        assertEquals("144", secure.value)
        now = 5_000
        assertEquals(0L, lift.due(true))
        assertEquals("user_refresh_rate 144 -> 90, the rate the session wants now", lift.settle(true))
        assertEquals("90", secure.value)
        assertEquals(Record("120", "90", "r1"), records.record)
        assertEquals("r1 120 90", secure.marker)
        assertNull(lift.due(true))

        lift.want(144, 144)
        assertEquals(5_000L, lift.due(true))
        now = 9_000
        assertNull(lift.settle(true))
        now = 10_000
        assertEquals("user_refresh_rate 90 -> 144, the rate the session wants now", lift.settle(true))
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)
        assertEquals(listOf<String?>("144", "90", "144", "120"), secure.writes)
        assertEquals(listOf("r1"), secure.armed)
    }

    @Test fun `a rate wanted for less than the steady time is not written`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        now = 10_000
        lift.want(90, 144)
        now = 11_000
        lift.want(144, 144)
        assertNull(lift.due(true))
        now = 20_000
        assertNull(lift.settle(true))
        lift.want(60, 144)
        now = 21_999
        assertNull(lift.settle(true))
        now = 22_500
        lift.want(120, 144)
        now = 23_000
        assertNull(lift.settle(true))
        assertEquals(1_500L, lift.due(true))
        assertEquals(listOf<String?>("144"), secure.writes)
    }

    @Test fun `nothing is due while hidden, with the setting off, or after a call that failed`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.want(90, 144)
        assertNull(lift.due(true))
        lift.shown(true, 144, 90)
        assertEquals("90", secure.value)
        lift.want(144, 144)
        now = 10_000
        assertNull(lift.due(false))
        assertEquals(0L, lift.due(true))
        lift.hidden()
        assertNull(lift.due(true))
        assertNull(lift.settle(true))

        secure.failing = true
        assertThrows(IllegalStateException::class.java) { lift.shown(true, 144, 144) }
        lift.want(90, 144)
        now = 20_000
        assertNull(lift.due(true))
        secure.failing = false
        assertEquals("user_refresh_rate 120 -> 90 while a session is on screen", lift.shown(true, 144, 90))
    }

    @Test fun `shown again while a change waits, the rate in place is checked and the change keeps its time`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        lift.want(90, 144)
        now = 1_000
        val reads = secure.reads
        assertNull(lift.shown(true, 144, 90))
        assertEquals(reads + 1, secure.reads)
        assertEquals("144", secure.value)
        assertEquals(4_000L, lift.due(true))
        now = 5_000
        assertEquals("user_refresh_rate 144 -> 90, the rate the session wants now", lift.shown(true, 144, 90))
        assertEquals("90", secure.value)
    }

    @Test fun `the restorer puts the value back after a change, and the next process only drops its record`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        lift.want(90, 144)
        now = 5_000
        lift.settle(true)
        secure.processDied()
        assertEquals("120", secure.value)
        assertEquals("r1 120 90 restored", secure.marker)
        assertEquals("user_refresh_rate 120, changed from 90 meanwhile: left as it is", logic(secure).appStarted())
        assertNull(records.record)
        assertNull(secure.marker)
    }

    @Test fun `a change whose write did not go through is put back from the value before it`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        lift.want(90, 144)
        now = 5_000
        secure.failingChange = true
        assertThrows(IllegalStateException::class.java) { lift.settle(true) }
        assertEquals("144", secure.value)
        assertEquals(Record("120", "90", "r1", confirmed = false, previous = "144"), records.record)
        assertEquals("r1 120 90 144", secure.marker)
        assertNull(lift.due(true))
        assertTrue(lift.pending(true))
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)
    }

    @Test fun `a change that failed while shown is tried again with the rate wanted then`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 120 -> 90 while a session is on screen", lift.shown(true, 144, 90))
        assertFalse(lift.pending(true))
        lift.want(144, 144)
        now = 5_000
        secure.failing = true
        assertThrows(IllegalStateException::class.java) { lift.settle(true) }
        assertEquals("90", secure.value)
        assertNull(lift.due(true))
        assertTrue(lift.pending(true))
        assertThrows(IllegalStateException::class.java) { lift.retry(true) }
        assertTrue(lift.pending(true))
        secure.failing = false
        lift.want(120, 144)
        assertEquals("user_refresh_rate 90 -> 120, the rate the session wants now", lift.retry(true))
        assertEquals("120", secure.value)
        assertEquals(Record("120", "120", "r1"), records.record)
        assertFalse(lift.pending(true))
        assertNull(lift.retry(true))
        assertNull(lift.due(true))
        assertEquals(listOf<String?>("90", "120"), secure.writes)
    }

    @Test fun `a change whose write did not go through is tried again while shown`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        lift.want(90, 144)
        now = 5_000
        secure.failingChange = true
        assertThrows(IllegalStateException::class.java) { lift.settle(true) }
        assertTrue(lift.pending(true))
        secure.failingChange = false
        assertEquals("user_refresh_rate 144 -> 90, the rate the session wants now", lift.retry(true))
        assertEquals("90", secure.value)
        assertEquals(Record("120", "90", "r1"), records.record)
        assertEquals("r1 120 90", secure.marker)
        assertFalse(lift.pending(true))
        assertEquals("user_refresh_rate 90 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
    }

    @Test fun `with a change under way when the process died, the restorer puts back either value`() {
        for (left in listOf("144", "90")) {
            val secure = FakeSecure("120")
            records.record = null
            val lift = logic(secure)
            lift.shown(true, 144)
            lift.want(90, 144)
            now += 5_000
            secure.failingChange = true
            assertThrows(IllegalStateException::class.java) { lift.settle(true) }
            secure.value = left // the write went through after all, or not
            secure.processDied()
            assertEquals("120", secure.value)
            assertTrue(secure.marker!!.endsWith(" restored"))
        }
    }

    @Test fun `a change under way left by a process that died is put back at the next start, unless the owner changed it`() {
        for ((value, line) in listOf(
            "144" to "user_refresh_rate 144 -> 120 again",
            "90" to "user_refresh_rate 90 -> 120 again",
            "60" to "user_refresh_rate 60, changed from 90 meanwhile: left as it is",
        )) {
            val secure = FakeSecure(value)
            records.record = Record("120", "90", "r9", confirmed = false, previous = "144")
            assertEquals(line, logic(secure).appStarted())
            assertNull(records.record)
            assertEquals(if (value == "60") "60" else "120", secure.value)
        }
    }

    @Test fun `a change lost with the app's data is taken from the marker naming both values`() {
        val secure = FakeSecure("144")
        secure.marker = "r7 120 90 144"
        val lift = logic(secure)
        assertEquals("user_refresh_rate 144 recorded only in the restorer's marker: kept, the restorer now watches this process", lift.shown(true, 144))
        assertEquals(Record("120", "144", "r1"), records.record)
        assertEquals("r1 120 144", secure.marker)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
    }

    @Test fun `the owner's change while shown is the value put back after the next change`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        secure.value = "60"
        lift.want(90, 144)
        now = 5_000
        assertEquals("user_refresh_rate 60, changed from 144 meanwhile -> 90 while a session is on screen", lift.settle(true))
        assertEquals(Record("60", "90", "r2"), records.record)
        assertEquals("r2 60 90", secure.marker)
        // and again, to the owner's own value meanwhile: no record
        secure.value = "120"
        lift.want(120, 144)
        now = 12_000
        assertEquals("user_refresh_rate 120, changed from 90 meanwhile: already the 120 Hz the session wants", lift.settle(true))
        assertNull(records.record)
        assertNull(secure.marker)
        assertNull(lift.hidden())
        assertEquals("120", secure.value)
        assertEquals(listOf<String?>("144", "90"), secure.writes)
    }

    @Test fun `a rate the owner's value already is needs no record, and another one wanted later is written`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 120: already the 120 Hz the session wants", lift.shown(true, 144, 120))
        assertNull(records.record)
        lift.want(144, 144)
        assertEquals(2_000L, lift.due(true))
        now = 2_000
        assertEquals("user_refresh_rate 120 -> 144 while a session is on screen", lift.settle(true))
        assertEquals(Record("120", "144", "r1"), records.record)
        lift.want(120, 144)
        now = 7_000
        assertEquals("user_refresh_rate 144 -> 120, the rate the session wants now", lift.settle(true))
        assertEquals(Record("120", "120", "r1"), records.record)
        assertEquals("user_refresh_rate 120 -> 120 again", lift.hidden())
        assertNull(secure.marker)
    }

    @Test fun `a rate below the owner's is written for the session and put back`() {
        val secure = FakeSecure("144")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 144 -> 120 while a session is on screen", lift.shown(true, 144, 120))
        assertEquals(Record("144", "120", "r1"), records.record)
        assertEquals("user_refresh_rate 120 -> 144 again", lift.hidden())
        assertEquals("144", secure.value)
        assertEquals("user_refresh_rate 144: already reaches the panel's 144 Hz", lift.shown(true, 144))
        assertNull(records.record)
    }

    @Test fun `a new process keeps a value raised before and changes it to the rate wanted`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.armed.clear()
        val lift = logic(secure)
        assertEquals("user_refresh_rate 144 raised before: kept, the restorer now watches this process; " +
            "user_refresh_rate 144 -> 90, the rate the session wants now", lift.shown(true, 144, 90))
        assertEquals(Record("120", "90", "r2"), records.record)
        assertEquals("r2 120 90", secure.marker)
        assertEquals("user_refresh_rate 90 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
    }

    @Test fun `the rate wanted is one of the panel's modes`() {
        val modes = listOf(60f, 90f, 120f, 144f, 120.00001f)
        assertEquals(144, RefreshRateLiftLogic.modeFor(0f, modes))
        assertEquals(90, RefreshRateLiftLogic.modeFor(89.9f, modes))
        assertEquals(120, RefreshRateLiftLogic.modeFor(100f, modes))
        assertEquals(60, RefreshRateLiftLogic.modeFor(59.94f, listOf(59.94f, 120f)))
        assertEquals(60, RefreshRateLiftLogic.modeFor(30f, modes))
        assertEquals(144, RefreshRateLiftLogic.modeFor(165f, modes))
        assertEquals(0, RefreshRateLiftLogic.modeFor(90f, emptyList()))
    }

    @Test fun `the shell's answer is read as a rate or unset`() {
        assertNull(RefreshRateLiftLogic.parse("null\n"))
        assertEquals("120", RefreshRateLiftLogic.parse("120\n"))
        assertEquals("144.0", RefreshRateLiftLogic.parse(" 144.0 "))
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.parse("") }
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.parse("auto") }
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.parse("120\n60") }
        assertTrue(RefreshRateLiftLogic.same("144", "144.0"))
        assertFalse(RefreshRateLiftLogic.same("144", null))
        assertTrue(RefreshRateLiftLogic.same(null, null))
        assertTrue(RefreshRateLiftLogic.below(null, 144))
    }

    @Test fun `the marker is read back as written, and anything else as none`() {
        val record = Record(null, "144", "a1b2")
        assertEquals(RefreshRateLiftLogic.Marker("a1b2", null, "144", null), RefreshRateLiftLogic.parseMarker(RefreshRateLiftLogic.markerLine(record) + "\n"))
        assertEquals(RefreshRateLiftLogic.Marker("a1b2", "120", "144", "restored"), RefreshRateLiftLogic.parseMarker("a1b2 120 144 restored"))
        assertNull(RefreshRateLiftLogic.parseMarker(""))
        assertNull(RefreshRateLiftLogic.parseMarker("a1b2 120"))
        assertNull(RefreshRateLiftLogic.parseMarker("a1b2 120 144 maybe"))
        assertNull(RefreshRateLiftLogic.parseMarker("A1;B2 120 144"))
        assertNull(RefreshRateLiftLogic.parseMarker("a1b2 120 144 restored extra"))
        val changing = Record("120", "90", "a1b2", confirmed = false, previous = "144")
        assertEquals("a1b2 120 90 144", RefreshRateLiftLogic.markerLine(changing))
        assertEquals(RefreshRateLiftLogic.Marker("a1b2", "120", "90", null, "144"), RefreshRateLiftLogic.parseMarker("a1b2 120 90 144"))
        assertNull(RefreshRateLiftLogic.parseMarker("a1b2 120 90 1;4"))
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.changeCommand("/tmp/m", Record("120", "90", "a1b2")) }
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.changeCommand("/tmp/m", changing.copy(previous = "1 4")) }
        assertEquals("/data/local/tmp/refresh-lift.com.droiddeck.launcher.g2", RefreshRateLiftLogic.markerPath("com.droiddeck.launcher.g2"))
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.markerPath("a b") }
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.armCommand("/tmp/m", 1, Record("120", "144", "x'y")) }
    }
}
