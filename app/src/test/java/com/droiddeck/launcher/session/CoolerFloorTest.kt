package com.droiddeck.launcher.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CoolerFloorTest {
    private val dump = """
        Cached temperatures:
        	Temperature{mValue=99.0, mType=-1, mName=quiet_therm, mStatus=0}
        Current temperatures from HAL:
        	Temperature{mValue=86.5, mType=-1, mName=soc-thermal, mStatus=0}
        	Temperature{mValue=85.875, mType=-1, mName=gpu-ss-max, mStatus=0}
        	Temperature{mValue=39.7, mType=3, mName=quiet_therm, mStatus=0}
        	Temperature{mValue=-1.5, mType=2, mName=battery, mStatus=0}
        Current cooling devices from HAL:
        	CoolingDevice{mValue=12, mType=2, mName=cpufreq-cpu0}
    """.trimIndent()

    @Test fun `only the current section is read`() {
        val temps = CoolerFloor.readings(dump)
        assertEquals(39.7, temps["quiet_therm"]!!, 0.0)
        assertEquals(-1.5, temps["battery"]!!, 0.0)
        assertEquals(4, temps.size)
        assertEquals(emptyMap<String, Double>(), CoolerFloor.readings("nothing here"))
    }

    @Test fun `inside every limit holds the floor`() {
        assertNull(CoolerFloor.tooHot(CoolerFloor.readings(dump)))
    }

    @Test fun `a sensor at its limit stops it`() {
        val ok = mapOf("quiet_therm" to 40.0, "soc-thermal" to 85.0, "gpu-ss-max" to 80.0)
        assertEquals("quiet_therm 42.0", CoolerFloor.tooHot(ok + ("quiet_therm" to 42.0)))
        assertEquals("charger_therm 55.0", CoolerFloor.tooHot(ok + ("charger_therm" to 55.0)))
    }

    @Test fun `a required sensor missing stops it, an optional one does not`() {
        assertEquals("gpu-ss-max unreadable", CoolerFloor.tooHot(mapOf("quiet_therm" to 30.0, "soc-thermal" to 50.0)))
        assertNull(CoolerFloor.tooHot(mapOf("quiet_therm" to 30.0, "soc-thermal" to 50.0, "gpu-ss-max" to 50.0)))
    }

    @Test fun `resuming needs the margin`() {
        val t = mapOf("quiet_therm" to 41.5, "soc-thermal" to 85.0, "gpu-ss-max" to 80.0)
        assertNull(CoolerFloor.tooHot(t))
        assertEquals("quiet_therm 41.5", CoolerFloor.tooHot(t, CoolerFloor.RESUME_MARGIN_C))
    }

    @Test fun `the reply gives the handle, a refusal gives none`() {
        assertEquals(238, CoolerFloor.handleOf("Result: Parcel(00000000 000000ee   '........')"))
        assertEquals(0, CoolerFloor.handleOf("Result: Parcel(00000000 ffffffff   '........')"))
        assertEquals(0, CoolerFloor.handleOf("Result: Parcel(ffffffb6 00000000 'Security')"))
    }
}
