package com.droiddeck.launcher.session

/**
 * When to ask the Steam client to exit after a game requested from the launcher. The game has to start by
 * [startBy]. Once it has closed, the client is asked to exit after the game has been gone for [graceMs] with
 * nothing listed by `busy` ([SteamContentActivity.busy]). While something is listed the return is held, for at
 * most [holdMaxMs] in all; a game running again starts over.
 */
internal class GameExitWatch(
    private val startBy: Long,
    private val graceMs: Long,
    private val holdMaxMs: Long,
) {
    sealed interface Step {
        data object Wait : Step
        /** The game is running for the first time. */
        data object Started : Step
        /** The game did not start by [startBy]; stop watching. */
        data object NotStarted : Step
        /** The game is gone and Steam is [busy]; [again] when this follows a [Done] for the same exit. */
        data class Held(val busy: List<String>, val again: Boolean) : Step
        /** What held the return has ended; the grace starts now. */
        data class Done(val held: List<String>) : Step
        /** Steam is still [busy] after [holdMaxMs]; ask the client to exit now. */
        data class Expired(val busy: List<String>) : Step
        /** Ask the client to exit now. */
        data object Return : Step
    }

    private var seen = false
    private var goneSince: Long? = null
    private var holdSince: Long? = null
    private var holding = false
    private val held = LinkedHashSet<String>()

    /** [busy] is only called once the game has run and is gone. */
    fun step(now: Long, gameRunning: Boolean, busy: () -> List<String>): Step {
        if (gameRunning) {
            goneSince = null
            holdSince = null
            holding = false
            held.clear()
            if (seen) return Step.Wait
            seen = true
            return Step.Started
        }
        if (!seen) return if (now > startBy) Step.NotStarted else Step.Wait
        val items = busy()
        if (items.isNotEmpty()) {
            held += items
            val since = holdSince
            if (since == null) {
                holdSince = now
                holding = true
                return Step.Held(items, again = false)
            }
            if (now - since >= holdMaxMs) return Step.Expired(items)
            if (!holding) {
                holding = true
                return Step.Held(items, again = true)
            }
            return Step.Wait
        }
        if (holding) {
            holding = false
            goneSince = now
            return Step.Done(held.toList()).also { held.clear() }
        }
        val gone = goneSince ?: now.also { goneSince = it }
        return if (now - gone >= graceMs) Step.Return else Step.Wait
    }
}
