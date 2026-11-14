package com.droiddeck.launcher.gpu

import android.content.Context
import com.droiddeck.launcher.R
import com.droiddeck.launcher.session.SessionState
import com.droiddeck.launcher.wayland.WaylandCompositor
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34], manifest = Config.NONE, instrumentedPackages = ["com.droiddeck.launcher.wayland.WaylandCompositor"],
    shadows = [FrameGenZeroFgTest.FakeCompositor::class])
class FrameGenZeroFgTest {
    private lateinit var context: Context
    private val previousRunning = SessionState.running
    private val previousZeroCopy = SessionState.zeroCopy

    @Before fun setUp() {
        context = RuntimeEnvironment.getApplication()
        context.getSharedPreferences("frame_gen", Context.MODE_PRIVATE).edit().clear().commit()
        FakeCompositor.calls.clear()
        FakeCompositor.problem = ""
        FakeCompositor.failure = null
        FrameGen.zeroFgFailure = null
        SessionState.running = false
        SessionState.zeroCopy = null
    }

    @After fun restore() {
        SessionState.running = previousRunning
        SessionState.zeroCopy = previousZeroCopy
    }

    private fun prefs() = context.getSharedPreferences("frame_gen", Context.MODE_PRIVATE)

    @Test fun bothEnginesAreKept() {
        FrameGen.set(context, FrameGen.zeroFg(FrameGen.ZEROFG_ZERO))
        assertEquals(FrameGen.Mode(FrameGen.ENGINE_ZEROFG, 2, 0, FrameGen.ZEROFG_ZERO), FrameGen.mode(context))
        FrameGen.set(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO))
        assertEquals(FrameGen.Mode(FrameGen.ENGINE_ZEROFG, 2, 0, FrameGen.ZEROFG_REALLY_ZERO), FrameGen.mode(context))
    }

    @Test fun reallyZeroIsTheDefault() {
        prefs().edit().putString("engine", FrameGen.ENGINE_ZEROFG).commit()
        assertEquals(FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), FrameGen.mode(context))
        prefs().edit().putString("zerofg", "turbo").commit()
        assertEquals(FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), FrameGen.mode(context))
        assertEquals(FrameGen.ZEROFG_REALLY_ZERO, FrameGen.zeroFg("").zeroFg)
    }

    @Test fun alwaysTimesTwoWithoutATarget() {
        prefs().edit().putString("engine", FrameGen.ENGINE_ZEROFG).putString("zerofg", FrameGen.ZEROFG_ZERO)
            .putInt("multiplier", 4).putInt("target", 90).commit()
        assertEquals(FrameGen.Mode(FrameGen.ENGINE_ZEROFG, 2, 0, FrameGen.ZEROFG_ZERO), FrameGen.mode(context))
    }

    @Test fun otherEnginesCarryNoZeroFgEngine() {
        FrameGen.set(context, FrameGen.zeroFg(FrameGen.ZEROFG_ZERO))
        FrameGen.set(context, FrameGen.Mode(FrameGen.ENGINE_LSFG, 3))
        assertEquals(FrameGen.Mode(FrameGen.ENGINE_LSFG, 3), FrameGen.mode(context))
        FrameGen.set(context, FrameGen.Mode(FrameGen.ENGINE_DDFG, 2))
        assertEquals(FrameGen.Mode(FrameGen.ENGINE_DDFG, 2), FrameGen.mode(context))
    }

    @Test fun theMenuOffersReallyZeroThenZero() {
        assertEquals(
            listOf(FrameGen.Mode(FrameGen.ENGINE_ZEROFG, 2, 0, FrameGen.ZEROFG_REALLY_ZERO),
                FrameGen.Mode(FrameGen.ENGINE_ZEROFG, 2, 0, FrameGen.ZEROFG_ZERO)),
            FrameGen.ZEROFG_MENU,
        )
    }

    @Test fun labels() {
        assertEquals("ZeroFG ReallyZero 2×", FrameGen.label(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO)))
        assertEquals("ZeroFG Zero 2×", FrameGen.label(context, FrameGen.zeroFg(FrameGen.ZEROFG_ZERO)))
    }

    @Test fun zeroArmsTheZeroCopyEngine() {
        assertNull(FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_ZERO), 120f))
        assertEquals(listOf("engine ${WaylandCompositor.FG_ENGINE_ZEROFG}", "zerofg ${WaylandCompositor.ZEROFG_MODE_ZERO}",
            "tuning 120.0", "peak 120.0", "armed true 2 0"), FakeCompositor.calls)
        assertEquals(2, WaylandCompositor.FG_ENGINE_ZEROFG)
        assertEquals(1, WaylandCompositor.ZEROFG_MODE_ZERO)
    }

    @Test fun reallyZeroArmsItsEngine() {
        assertNull(FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), 144f))
        assertEquals(listOf("engine 2", "zerofg 0", "tuning 144.0", "peak 144.0", "armed true 2 0"), FakeCompositor.calls)
    }

    @Test fun theVoteIsCappedAtThePeakRateNotTheRunningOne() {
        assertNull(FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), 90f, 120f))
        assertEquals(listOf("engine 2", "zerofg 0", "tuning 90.0", "peak 120.0", "armed true 2 0"), FakeCompositor.calls)
        FakeCompositor.calls.clear()
        assertNull(FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), 120f, 60f))
        assertTrue(FakeCompositor.calls.toString(), "peak 120.0" in FakeCompositor.calls)
    }

    @Test fun aFailureInTheSessionIsSaidNotRemembered() {
        FakeCompositor.failure = "off for this session: ZeroFG's GPU device was lost"
        val said = FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_ZERO), 120f)
        assertTrue(said, said!!.contains("ZeroFG's GPU device was lost"))
        assertEquals(FakeCompositor.failure, FrameGen.zeroFgFailure)
        assertNull(FrameGen.zeroFgProblem(context))
        FakeCompositor.failure = null
        assertNull(FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_ZERO), 120f))
        assertNull(FrameGen.zeroFgFailure)
    }

    @Test fun aSessionWithoutZeroCopyIsSaid() {
        SessionState.running = true
        SessionState.zeroCopy = false
        assertEquals(context.getString(R.string.zerofg_needs_zero_copy),
            FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), 120f))
        assertEquals(false, FrameGen.zeroFgPathOk(context))
        SessionState.zeroCopy = true
        assertNull(FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), 120f))
        assertEquals(true, FrameGen.zeroFgPathOk(context))
    }

    @Test fun anUnsupportedGpuIsRememberedAndSaid() {
        FakeCompositor.problem = "the GPU has no synchronization2"
        val said = FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), 120f)
        assertTrue(said, said!!.contains("the GPU has no synchronization2"))
        assertEquals("the GPU has no synchronization2", FrameGen.zeroFgProblem(context))
        FakeCompositor.problem = ""
        assertNull(FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_REALLY_ZERO), 120f))
        assertNull(FrameGen.zeroFgProblem(context))
    }

    @Test fun anUncheckedGpuKeepsTheLastFinding() {
        FrameGen.noteZeroFgProblem(context, "no Vulkan 1.3")
        FakeCompositor.problem = null
        assertNull(FrameGen.apply(context, FrameGen.zeroFg(FrameGen.ZEROFG_ZERO), 120f))
        assertEquals("no Vulkan 1.3", FrameGen.zeroFgProblem(context))
    }

    @Test fun offDisarms() {
        assertNull(FrameGen.apply(context, FrameGen.Mode.OFF, 120f))
        assertEquals(listOf("armed false 0 0"), FakeCompositor.calls)
    }

    @Test fun knobsComeFromTheSessionEnvironment() {
        val env = FrameGen.zeroFgEnv(listOf("DROIDDECK_ZEROFG_DPT=0", "FOO=1", "DROIDDECK_ZEROFG_MODE=zero",
            "DROIDDECK_ZEROFG_MODE=reallyzero", "DROIDDECK_ZEROFG_MARGIN_MS=4", "DROIDDECK_ZEROFG_LATCH_MS=12"))
        assertEquals(FrameGen.ZEROFG_ENV.toSet(), env.keys)
        assertEquals("0", env["DROIDDECK_ZEROFG_DPT"])
        assertEquals("reallyzero", env["DROIDDECK_ZEROFG_MODE"])
        assertEquals("4", env["DROIDDECK_ZEROFG_MARGIN_MS"])
        assertEquals("12", env["DROIDDECK_ZEROFG_LATCH_MS"])
        assertNull(env["DROIDDECK_ZEROFG_PRIORITY"])
    }

    @Implements(value = WaylandCompositor::class, isInAndroidSdk = false)
    class FakeCompositor {
        companion object {
            val calls = mutableListOf<String>()
            var problem: String? = ""
            var failure: String? = null

            @JvmStatic @Implementation fun __staticInitializer__() {}
            @JvmStatic @Implementation fun nativeSetFrameGenEngine(kind: Int) { calls += "engine $kind" }
            @JvmStatic @Implementation fun nativeSetZeroFgMode(mode: Int) { calls += "zerofg $mode" }
            @JvmStatic @Implementation fun nativeSetFrameGenTuning(flowScale: Float, refreshHz: Float) { calls += "tuning $refreshHz" }
            @JvmStatic @Implementation fun nativeSetFrameGenArmed(armed: Boolean, multiplier: Int, targetFps: Int) {
                calls += "armed $armed $multiplier $targetFps"
            }
            @JvmStatic @Implementation fun nativeSetDdFgTuning(model: Int, perfPreset: Int) { calls += "ddfg $model $perfPreset" }
            @JvmStatic @Implementation fun nativeZeroFgProblem(): String? = problem
            @JvmStatic @Implementation fun nativeSetZeroFgPeakHz(hz: Float) { calls += "peak $hz" }
            @JvmStatic @Implementation fun nativeZeroFgFailure(): String? = failure
            @JvmStatic @Implementation fun nativeZeroFgNewSession() { calls += "new session" }
        }
    }
}
