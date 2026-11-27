package com.droiddeck.launcher.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ThermalBreakLimitTest {
    private val via = "shell user, through the wireless debugging pairing"

    @Test fun `the owner's value is logged without a warning`() {
        val report = ThermalBreakLimit.report("50000\n", via)
        assertFalse(report.warning)
        assertEquals("thermal break limit: persist.sys.miuibooster.thermalbreaklimit=50000 ($via)", report.line)
    }

    @Test fun `an unset value is a warning and names the default gate`() {
        val report = ThermalBreakLimit.report("\n", via)
        assertTrue(report.warning)
        assertEquals("thermal break limit: persist.sys.miuibooster.thermalbreaklimit is unset ($via), not 50000: " +
            "MiBridge refuses CPU floors from 40 C on the board sensor; system_server reads it at boot only", report.line)
    }

    @Test fun `another value is a warning with its temperature`() {
        assertTrue(ThermalBreakLimit.report("40000", via).line.contains("=40000 ($via), not 50000: MiBridge refuses CPU floors from 40 C"))
        assertTrue(ThermalBreakLimit.report("45500", "read by the app").line.contains("from 45.5 C"))
        val odd = ThermalBreakLimit.report("50", via)
        assertTrue(odd.warning)
        assertTrue(odd.line.contains("=50 ($via), not 50000"))
        val text = ThermalBreakLimit.report("high", via)
        assertTrue(text.warning)
        assertTrue(text.line.contains("=high ($via), not 50000; system_server"))
    }

    @Test fun `a value that could not be read is no warning`() {
        val report = ThermalBreakLimit.report(null, "wireless debugging is off")
        assertFalse(report.warning)
        assertEquals("thermal break limit: persist.sys.miuibooster.thermalbreaklimit not read (wireless debugging is off)", report.line)
    }

    @Test fun `the property is only read`() {
        assertEquals("getprop persist.sys.miuibooster.thermalbreaklimit", ThermalBreakLimit.COMMAND)
    }
}
