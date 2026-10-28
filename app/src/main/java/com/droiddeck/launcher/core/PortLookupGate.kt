package com.droiddeck.launcher.core

/**
 * [mali-g2] One wireless debugging port lookup at a time, and a failed one remembered for [ttlMs].
 *
 * When the saved pairing stops working, the callers at a session's start (the launch and session checks of
 * HyperOsGameMode, MiBridgeBoost) each used to look for a new port in turn, mDNS and then a loopback scan, about
 * 14 s apiece. Callers that come during a lookup now wait for it; when it found nothing, they and any caller
 * within [ttlMs] get its result without looking again. A lookup that found a port saves it, so later callers
 * connect to that directly; [clear] forgets a failure once any connection through the pairing works again.
 *
 * A failure is remembered per [K] (the saved connection), so a new pairing or a saved port another caller
 * found is looked up afresh. A lookup cut short by an interrupt is not remembered.
 */
internal class PortLookupGate<K : Any>(private val ttlMs: Long, private val clock: () -> Long) {
    /** Held for a whole lookup. */
    private val running = Any()
    /** Guards [failed] and [failedAt]; never held across a lookup. */
    private val state = Any()
    private var failed: K? = null
    private var failedAt = 0L

    /**
     * [lookup] for the key [key] reads once the gate is free, or null without it: when [key] gives null, or when
     * the lookup for the same key failed less than [ttlMs] ago ([onSkipped] gets how long ago).
     */
    fun <T : Any> run(key: () -> K?, onSkipped: (ageMs: Long) -> Unit = {}, lookup: (K) -> T?): T? =
        synchronized(running) {
            val current = key() ?: return null
            failedAgo(current)?.let { age ->
                onSkipped(age)
                return null
            }
            val result = lookup(current)
            synchronized(state) {
                if (result != null) {
                    failed = null
                } else if (!Thread.currentThread().isInterrupted) {
                    failed = current
                    failedAt = clock()
                }
            }
            result
        }

    /** [block] while no lookup runs, without the remembered failure: for a lookup the user asked for. */
    fun <T> exclusive(block: () -> T): T = synchronized(running) { block() }

    /** How long ago the lookup for [key] failed, when that is less than [ttlMs]; else null. */
    fun failedAgo(key: K): Long? = synchronized(state) {
        if (failed != key) return null
        (clock() - failedAt).takeIf { it in 0 until ttlMs }
    }

    /** Forgets a failure: a connection through the pairing worked, or the pairing changed. */
    fun clear() = synchronized(state) { failed = null }
}
