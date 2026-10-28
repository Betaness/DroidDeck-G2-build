package com.droiddeck.launcher.core

import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PortLookupGateTest {
    private val now = AtomicLong(1_000_000L)
    private val gate = PortLookupGate<String>(ttlMs = 60_000L) { now.get() }
    private val lookups = AtomicInteger()

    private fun lookUp(key: String, result: Int?, skipped: MutableList<Long> = mutableListOf()): Int? =
        gate.run({ key }, { skipped += it }) {
            assertEquals(key, it)
            lookups.incrementAndGet()
            result
        }

    @Test fun `a lookup that found nothing answers for the same key until the time is up`() {
        assertNull(lookUp("127.0.0.1:40001", null))
        now.addAndGet(59_999L)
        val skipped = mutableListOf<Long>()
        assertNull(lookUp("127.0.0.1:40001", 1234, skipped))
        assertEquals(1, lookups.get())
        assertEquals(listOf(59_999L), skipped)
        assertEquals(59_999L, gate.failedAgo("127.0.0.1:40001"))

        now.addAndGet(1L)
        assertNull(gate.failedAgo("127.0.0.1:40001"))
        assertEquals(1234, lookUp("127.0.0.1:40001", 1234))
        assertEquals(2, lookups.get())
    }

    @Test fun `another key is looked up again`() {
        assertNull(lookUp("127.0.0.1:40001", null))
        assertNull(lookUp("127.0.0.1:", null)) // paired again: the saved port is gone
        assertEquals(2, lookups.get())
        // Only the last failure is remembered.
        assertNull(gate.failedAgo("127.0.0.1:40001"))
        assertEquals(0L, gate.failedAgo("127.0.0.1:"))
    }

    @Test fun `a connection that works forgets the failure`() {
        assertNull(lookUp("k", null))
        gate.clear()
        assertNull(gate.failedAgo("k"))
        assertEquals(7, lookUp("k", 7))
        assertEquals(2, lookups.get())
    }

    @Test fun `a lookup that found a port is not remembered as a failure`() {
        assertNull(lookUp("k", null))
        now.addAndGet(60_000L)
        assertEquals(5, lookUp("k", 5))
        // A later failure of the found port is looked up again: the saved port changed with the success, and
        // were it the same key, the success cleared the old failure.
        assertEquals(6, lookUp("k", 6))
        assertEquals(3, lookups.get())
    }

    @Test fun `without a key nothing is looked up`() {
        assertNull(gate.run({ null }) { lookups.incrementAndGet() })
        assertEquals(0, lookups.get())
    }

    @Test fun `a clock that went back does not keep a failure`() {
        assertNull(lookUp("k", null))
        now.addAndGet(-1L)
        assertNull(gate.failedAgo("k"))
    }

    @Test fun `an interrupted lookup is not remembered`() {
        assertNull(gate.run<Int>({ "k" }) {
            lookups.incrementAndGet()
            Thread.currentThread().interrupt()
            null
        })
        assertTrue(Thread.interrupted()) // clears it for the next test
        assertNull(gate.failedAgo("k"))
        assertEquals(3, lookUp("k", 3))
    }

    @Test fun `callers that come during a lookup wait for it and share its failure`() {
        val started = CountDownLatch(1)
        val release = CountDownLatch(1)
        val results = Collections.synchronizedList(mutableListOf<Int?>())
        val first = Thread {
            results += gate.run<Int>({ "k" }) {
                lookups.incrementAndGet()
                started.countDown()
                release.await(5, TimeUnit.SECONDS)
                null
            }
        }.apply { start() }
        assertTrue(started.await(5, TimeUnit.SECONDS))
        val waiting = List(2) {
            Thread { results += lookUp("k", 99) }.apply { start() }
        }
        // They queue behind the running lookup; the failure state stays readable meanwhile.
        Thread.sleep(100)
        assertNull(gate.failedAgo("k"))
        gate.clear()
        assertEquals(1, lookups.get())
        release.countDown()
        (waiting + first).forEach { it.join(5_000); assertFalse(it.isAlive) }

        assertEquals(1, lookups.get())
        assertEquals(listOf<Int?>(null, null, null), results.toList())
    }

    @Test fun `a lookup the user asked for runs alone but past a failure`() {
        assertNull(lookUp("k", null))
        assertEquals("ran", gate.exclusive { "ran" })
        // It does not change what is remembered.
        assertEquals(0L, gate.failedAgo("k"))
    }
}
