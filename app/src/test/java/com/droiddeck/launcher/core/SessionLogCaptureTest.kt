package com.droiddeck.launcher.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionLogCaptureTest {
    @Test fun vmstatKeepsTheReclaimAndSwapCounters() {
        val text = "nr_free_pages 1000\npswpin 12\npswpout 34\npgscan_kswapd 5\npgscan_direct 6\n" +
            "allocstall_normal 7\nworkingset_refault_anon 8\noom_kill 0\npgfault 99\n"
        assertEquals("pswpin 12\npswpout 34\npgscan_kswapd 5\npgscan_direct 6\nallocstall_normal 7\n" +
            "workingset_refault_anon 8\noom_kill 0\n", SessionLogCapture.vmstatLines(text))
        assertEquals("unavailable\n", SessionLogCapture.vmstatLines("nr_free_pages 1\n"))
    }

    /** Upstream #2758: lmkd's records come over Wireless debugging; without a pairing the file says so. */
    @Test fun theKillRecordsNeedAPairing() {
        assertTrue(SessionLogCapture.lowMemoryKiller(false) { error("not read") }.startsWith("(not captured: no Wireless debugging pairing"))
        assertEquals("killinfo: [1234,10234,950,...]\n", SessionLogCapture.lowMemoryKiller(true) { "killinfo: [1234,10234,950,...]\n" })
        assertEquals("(no lmkd or kill lines in the log)\n", SessionLogCapture.lowMemoryKiller(true) { "" })
        assertEquals("(not captured: refused)\n", SessionLogCapture.lowMemoryKiller(true) { error("refused") })
    }

    @Test fun theCommandAsksForLmkdAndSystemKills() {
        for (tag in listOf("lowmemorykiller:D", "killinfo:I", "am_kill:I", "-b main,events"))
            assertTrue(tag, SessionLogCapture.LMKD_COMMAND.contains(tag))
    }
}
