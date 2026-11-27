package com.droiddeck.launcher.wayland

import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.view.Choreographer
import android.view.Surface
import androidx.annotation.RequiresApi
import com.droiddeck.launcher.session.SessionPrefs
import com.droiddeck.launcher.session.SessionState

object CompositorHost {
    @Volatile
    private var started = false

    @Volatile
    var displayChoice: String? = null

    @Volatile
    private var vsyncRunning = false

    @Volatile
    private var ownVsyncThread = true

    private val vsyncThread: HandlerThread by lazy {
        HandlerThread("droiddeck-vsync", Process.THREAD_PRIORITY_DISPLAY).apply { start() }
    }
    private val vsyncHandler: Handler by lazy { Handler(vsyncThread.looper) }

    @Volatile
    private var vsyncGeneration = 0

    /** [mali-g2] Even frame pacing is on (the activity's choice, while the game is on the phone screen). */
    @Volatile
    private var presentCadence = false

    /** [mali-g2] With even frame pacing on API 33+: the tick also hands the compositor the preferred frame timeline. */
    @RequiresApi(Build.VERSION_CODES.TIRAMISU)
    private fun timelineCallback(generation: Int) = object : Choreographer.VsyncCallback {
        override fun onVsync(data: Choreographer.FrameData) {
            if (generation != vsyncGeneration) return
            val lines = data.frameTimelines
            val line = data.preferredFrameTimeline
            val period = if (lines.size > 1) lines[1].expectedPresentationTimeNanos - lines[0].expectedPresentationTimeNanos else 0L
            WaylandCompositor.nativeFrameTimeline(line.expectedPresentationTimeNanos, line.deadlineNanos, period)
            WaylandCompositor.nativeVsync(data.frameTimeNanos)
            if (vsyncRunning) Choreographer.getInstance().postVsyncCallback(this)
        }
    }

    private fun postTick(generation: Int) {
        if (presentCadence && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            Choreographer.getInstance().postVsyncCallback(timelineCallback(generation))
        } else {
            Choreographer.getInstance().postFrameCallback(vsyncCallback(generation))
        }
    }

    private fun vsyncCallback(generation: Int) = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (generation != vsyncGeneration) return
            WaylandCompositor.nativeVsync(frameTimeNanos)
            if (vsyncRunning) Choreographer.getInstance().postFrameCallback(this)
        }
    }

    @Synchronized
    fun startOrAttach(
        context: android.content.Context,
        surface: Surface,
        xdgRuntimeDir: String,
        driverPath: String?,
        libraryName: String?,
        nativeLibDir: String,
        outputWidth: Int,
        outputHeight: Int,
        refreshHz: Float,
        fpsLimit: Int,
    ): Boolean {
        ownVsyncThread = SessionPrefs.vsyncThread(context)
        WaylandCompositor.nativeSetRenderOnCommit(SessionPrefs.renderOnCommit(context))
        WaylandCompositor.nativeSetPresentCadence(presentCadence)
        pace(refreshHz, fpsLimit)
        if (!SessionState.running) WaylandCompositor.nativeSetOutputSize(outputWidth, outputHeight)
        if (started) {
            attached = surface
            WaylandCompositor.nativeSetSurface(surface)
            resumeVsync()
            return false
        }
        com.droiddeck.launcher.gpu.TpUbwcHint.applyToProcess(context)
        WaylandCompositor.nativeStartWithSurface(
            surface, xdgRuntimeDir, driverPath, libraryName, nativeLibDir,
        )
        attached = surface
        started = true
        resumeVsync()
        return true
    }

    /** [voteHz]: the layers' frame-rate vote without an fps limit (with ZeroFG: a rate its own vote did not lower). */
    private fun pace(refreshHz: Float, fpsLimit: Int, voteHz: Float = refreshHz) {
        WaylandCompositor.nativeSetFpsLimit(fpsLimit)
        WaylandCompositor.nativeSetLayerFrameRate(if (fpsLimit > 0) fpsLimit.toFloat() else voteHz)
        WaylandCompositor.nativeSetOutputRefreshRate(refreshHz)
        SessionState.refreshHz = refreshHz
    }

    @Synchronized
    fun updatePacing(refreshHz: Float, fpsLimit: Int, voteHz: Float = refreshHz) {
        if (started) pace(refreshHz, fpsLimit, voteHz)
    }

    /** [mali-g2] Even frame pacing on or off; the tick restarts so it carries the frame timeline only while on. */
    @Synchronized
    fun setPresentCadence(on: Boolean) {
        if (presentCadence == on) return
        presentCadence = on
        if (!started) return
        WaylandCompositor.nativeSetPresentCadence(on)
        if (vsyncRunning) {
            pauseVsync()
            resumeVsync()
        }
    }

    @Synchronized
    fun resize(surface: Surface, rearm: () -> Unit) {
        if (!started) return
        WaylandCompositor.nativeSetFrameGenArmed(false, 0, 0)
        WaylandCompositor.nativeLog("screen", "surface resized: rebinding the window, frame generation re-armed after")
        WaylandCompositor.nativeSetSurface(null)
        attached = surface
        WaylandCompositor.nativeSetSurface(surface)
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(rearm, 600)
    }

    fun rearmFrameGen(rearm: () -> Unit) {
        if (!started) return
        WaylandCompositor.nativeSetFrameGenArmed(false, 0, 0)
        WaylandCompositor.nativeLog("screen", "presenting window changed: frame generation re-armed")
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(rearm, 600)
    }

    private var attached: Surface? = null

    @Synchronized
    fun attachXr() {
        check(started) { "DroidDeck compositor is not running" }
        pauseVsync()
        attached = null
        WaylandCompositor.nativeSetSurface(null)
    }

    @Synchronized
    fun detach(surface: Surface?) {
        if (!started) return
        if (surface != null && attached != null && attached !== surface) return
        attached = null
        pauseVsync()
        WaylandCompositor.nativeSetSurface(null)
    }

    private fun resumeVsync() {
        if (vsyncRunning) return
        val generation = ++vsyncGeneration
        vsyncRunning = true
        WaylandCompositor.nativeLog("screen", if (ownVsyncThread) "screen tick runs on its own display-priority thread" else "screen tick runs on the main thread")
        if (!ownVsyncThread) {
            postTick(generation)
            return
        }
        vsyncHandler.post { postTick(generation) }
    }

    private fun pauseVsync() {
        vsyncRunning = false
    }

    fun newSession() {
        com.droiddeck.launcher.gpu.FrameGen.zeroFgFailure = null
        if (started) {
            WaylandCompositor.nativeResetFirstFrame()
            WaylandCompositor.nativeZeroFgNewSession()
        }
    }

    val isStarted: Boolean get() = started
}
