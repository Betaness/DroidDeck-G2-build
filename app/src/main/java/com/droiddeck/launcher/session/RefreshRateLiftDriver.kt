package com.droiddeck.launcher.session

import kotlin.math.abs

/**
 * [mali-g2] The order of [RefreshRateLift]'s calls, without Android. Every call of [RefreshRateLiftLogic] runs on
 * [scheduler]'s one thread, in order; the logic is made there on first use ([newLogic]). [enabled] reads the setting,
 * [modes] the default display's refresh rates (empty when unknown).
 *
 * A change of the rate wanted is planned for when [RefreshRateLiftLogic.due] tells, after every call. A call that failed
 * is tried again after [RETRY_MS], one wait per failure in a row, and [WIRELESS_DEBUGGING_ON_MS] after wireless
 * debugging comes on, while [RefreshRateLiftLogic.pending] tells that a value written here waits to be put back or,
 * while shown, to be replaced by the rate wanted.
 */
internal class RefreshRateLiftDriver(
    private val scheduler: Scheduler,
    private val enabled: () -> Boolean,
    private val modes: () -> List<Float>,
    private val events: Events,
    private val newLogic: () -> RefreshRateLiftLogic,
) {
    interface Scheduler {
        fun execute(task: () -> Unit)
        fun schedule(delayMs: Long, task: () -> Unit): Cancel
    }

    fun interface Cancel {
        /** Stops the task unless it has started. */
        fun cancel()
    }

    interface Events {
        /** A line for the log. */
        fun line(text: String)
        /** A call failed, the first since the driver was made or a session ended. */
        fun failed(t: Throwable)
        /** A call of the logic is over: its shell connection may close. */
        fun callDone()
        /** Whether to tell [debuggingCameOn] when wireless debugging comes on. */
        fun watchDebugging(on: Boolean)
    }

    /** The panel's top rate and its mode for the rate wanted; 0 when unknown. */
    data class Rates(val top: Int, val hz: Int)

    /** The rate a session wants of the built-in panel (0: none in particular, the top rate), and why. */
    data class Want(val hz: Float, val why: String)

    // The scheduler's thread only.
    private var logic: RefreshRateLiftLogic? = null
    private var failureLogged = false
    private var retries = 0
    private var retry: Cancel? = null
    private var change: Cancel? = null

    // Callers' threads, under this object's lock.
    private var pendingHide: Cancel? = null

    /** The rate the session asks for ([want]); 0: nothing in particular. */
    @Volatile
    var wantedHz = 0f
        private set

    /** A session activity is on screen: started and not in picture-in-picture. */
    @Synchronized
    fun shown() {
        pendingHide?.cancel()
        pendingHide = null
        submit { logic -> rates().let { logic.shown(enabled(), it.top, it.hz) } }
    }

    /** The rate the session asks the panel for, [hz] (1 and below: nothing in particular, the top rate). */
    fun want(hz: Float) {
        val next = if (hz > 1f) hz else 0f
        if (abs(next - wantedHz) < 0.01f) return
        wantedHz = next
        scheduler.execute {
            val logic = logic()
            rates().let { logic.want(it.hz, it.top) }
            plan(logic)
        }
    }

    /** No session activity is on screen any more; [grace] lets one that is recreated keep the setting raised. */
    @Synchronized
    fun hidden(grace: Boolean) {
        pendingHide?.cancel()
        pendingHide = null
        if (!grace) {
            submit { it.hidden() }
            return
        }
        pendingHide = scheduler.schedule(HIDE_GRACE_MS) { call { it.hidden() } }
    }

    /** The device shuts down or restarts: the value goes back now, also with a session on screen; [done] once tried. */
    @Synchronized
    fun shutdown(done: () -> Unit) {
        pendingHide?.cancel()
        pendingHide = null
        scheduler.execute {
            try {
                call { it.hidden() }
            } finally {
                done()
            }
        }
    }

    fun sessionStarted(on: Boolean) = submit { logic -> rates().let { logic.sessionStarted(on, it.top, it.hz) } }

    fun sessionEnded() {
        submit { it.sessionEnded() }
        scheduler.execute { failureLogged = false }
    }

    fun appStarted() = submit { it.appStarted() }

    fun turnedOff() = submit { it.turnedOff() }

    /** Wireless debugging came on: a retry that waits runs once adbd listens. */
    fun debuggingCameOn() {
        scheduler.schedule(WIRELESS_DEBUGGING_ON_MS) {
            val waiting = retry ?: return@schedule
            waiting.cancel()
            retry = null
            retryNow()
        }
    }

    private fun rates() = rates(wantedHz, runCatching { modes() }.getOrDefault(emptyList()))

    private fun logic() = logic ?: newLogic().also { logic = it }

    private fun submit(block: (RefreshRateLiftLogic) -> String?) = scheduler.execute { call(block) }

    private fun call(block: (RefreshRateLiftLogic) -> String?) {
        val logic = logic()
        var failure: Throwable? = null
        try {
            block(logic)?.let(events::line)
        } catch (t: Throwable) {
            failure = t
            if (!failureLogged) {
                failureLogged = true
                events.failed(t)
            }
        } finally {
            events.callDone()
        }
        settleRetry(logic, failure)
        plan(logic)
    }

    /** Has the rate wanted written once it is due ([RefreshRateLiftLogic.due]). */
    private fun plan(logic: RefreshRateLiftLogic) {
        change?.cancel()
        change = null
        val delay = runCatching { logic.due(enabled()) }.getOrNull() ?: return
        change = scheduler.schedule(delay) { call { it.settle(enabled()) } }
    }

    /** After a call: tries again later when it failed and [RefreshRateLiftLogic.pending], else stops trying. */
    private fun settleRetry(logic: RefreshRateLiftLogic, failure: Throwable?) {
        retry?.cancel()
        retry = null
        val on = runCatching { enabled() }.getOrDefault(false)
        if (failure == null || !runCatching { logic.pending(on) }.getOrDefault(false)) {
            retries = 0
            events.watchDebugging(false)
            return
        }
        val delay = RETRY_MS[retries.coerceAtMost(RETRY_MS.size - 1)]
        if (retries == 0) {
            val what = if (logic.visible && on) "not set to the rate the session wants" else "not put back"
            events.line("${RefreshRateLiftLogic.KEY} $what (${failure.message}): trying again in ${delay / 1000} s, " +
                "then less often, and when wireless debugging comes on")
        }
        retries++
        retry = scheduler.schedule(delay) { retryNow() }
        events.watchDebugging(true)
    }

    private fun retryNow() = call { it.retry(enabled()) }

    companion object {
        /** Session activities stopping and starting again (recreated, a dialog over them) keep the setting raised. */
        const val HIDE_GRACE_MS = 1_500L
        /** Waits before trying a call that failed again, one per failure in a row; the last repeats. */
        val RETRY_MS = longArrayOf(15_000L, 30_000L, 60_000L, 120_000L, 300_000L, 600_000L, 1_200_000L, 2_400_000L, 3_600_000L)
        /** After wireless debugging comes on, for adbd to listen. */
        const val WIRELESS_DEBUGGING_ON_MS = 3_000L

        /** The top rate among [modes] and the mode for [wantedHz] ([RefreshRateLiftLogic.modeFor]). */
        fun rates(wantedHz: Float, modes: Collection<Float>) =
            Rates(top = RefreshRateLiftLogic.modeFor(0f, modes), hz = RefreshRateLiftLogic.modeFor(wantedHz, modes))

        /**
         * The rate a session wants of the built-in panel. ZeroFG's frame-rate vote while ZeroFG runs ([zeroFg]) and the
         * game is shown on no other screen ([bottomHz], [glassesHz]: the rate of the bottom screen or the glasses
         * showing it, 0 when none does); [vote] is read only then. Else [mode] (the requested mode) while a frame rate
         * limit ([limitFps]) or the mirror ([mirrorHz]) picks it. Else none in particular.
         */
        fun sessionWant(
            zeroFg: Boolean,
            vote: () -> Float,
            bottomHz: Float,
            glassesHz: Float,
            limitFps: Int,
            mirrorHz: Int,
            mode: () -> Float,
        ): Want {
            val elsewhere = bottomHz > 1f || glassesHz > 1f
            val voted = if (zeroFg && !elsewhere) runCatching { vote() }.getOrDefault(0f) else 0f
            return when {
                voted > 1f -> Want(voted, "ZeroFG's frame-rate vote")
                limitFps > 0 || mirrorHz > 0 ->
                    Want(runCatching { mode() }.getOrDefault(0f), if (limitFps > 0) "the mode that divides $limitFps fps" else "the mirror's rate")
                else -> Want(0f, "the panel's top rate")
            }
        }
    }
}
