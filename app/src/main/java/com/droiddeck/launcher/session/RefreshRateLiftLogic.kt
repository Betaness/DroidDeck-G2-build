package com.droiddeck.launcher.session

import kotlin.math.abs

/**
 * [mali-g2] What [RefreshRateLift] does with HyperOS's refresh rate setting for apps (Secure user_refresh_rate), without
 * Android: [secure] reads and writes the setting and the restorer's marker, [records] keeps what this app wrote. Every
 * call returns a line for the log, or null when there is nothing to tell. A call that throws (the setting could not be
 * read or written) leaves the record as it was, so a later call tries again.
 *
 * While a session activity is on screen and the setting is on, the setting holds the rate the session wants ([want]):
 * a mode of the panel's, its top rate when nothing more particular is wanted (a value at or above the top rate is then
 * left as it is). Before the first write the value before is recorded (null when it was unset) and a restorer is started
 * as the shell user ([armCommand]): it outlives this process and, once the process is gone, puts the value before back
 * if the setting still holds a value written here, then notes in its marker what it did. The record is marked confirmed
 * once the write went through.
 *
 * Another rate wanted later replaces the one written once it has been wanted for [STEADY_MS] without a break, and no
 * sooner than [DWELL_MS] after the last write ([due], [settle]). Until that write went through, the record and the
 * restorer's marker name both rates ([changeCommand]); the restorer reads the marker when it acts.
 *
 * This app puts the value back when no session activity is on screen any more, the session ends or the setting is
 * turned off, and on the next app or session start while none is on screen; only while the setting still holds a
 * value written here, the restorer has not acted on it, and the write was confirmed or started by this process.
 * Otherwise the setting was changed meanwhile, and only the record goes. A record lost with the app's data is taken
 * from the marker while the setting still holds a value written and no restorer acted on it.
 */
internal class RefreshRateLiftLogic(
    private val secure: Secure,
    private val records: Records,
    /** Milliseconds, monotonic. */
    private val clock: () -> Long = { 0L },
    private val newId: () -> String,
) {
    interface Secure {
        /** The current value, null when unset; throws when it cannot be read. */
        fun read(): String?
        fun put(value: String)
        /** Puts [original] in place (deletes the setting for null) and removes the marker, which stops the restorer. */
        fun putBack(original: String?)
        /** What the restorer's marker holds, null when there is none. */
        fun marker(): Marker?
        /** Writes the marker for [record] and starts a restorer watching this process; an earlier restorer stops. */
        fun arm(record: Record)
        /** Puts [record]'s written value in place of its previous one, with the marker naming both until the write went through. */
        fun change(record: Record)
        /** Removes the marker, which stops the restorer. */
        fun disarm()
    }

    interface Records {
        fun load(): Record?
        fun save(record: Record)
        fun clear()
    }

    /**
     * [written] was put in place of [original] (null: it was unset), watched by the restorer [id]; [confirmed] once the
     * write went through. [previous]: the value written before, which the setting may still hold while the write of
     * [written] has not gone through.
     */
    data class Record(val original: String?, val written: String, val id: String, val confirmed: Boolean = true, val previous: String? = null)

    /**
     * The restorer's marker: the record it watches and, once the process it watched is gone, what it did ([RESTORED] or
     * [LEFT]); [previous] while a change is under way.
     */
    data class Marker(val id: String, val original: String?, val written: String, val outcome: String?, val previous: String? = null)

    /** A session activity is shown. */
    var visible = false
        private set

    /** The record whose write this process started, until it is confirmed. */
    private var attempted: Record? = null

    /** The restorer this process started, while its record stands. */
    private var armed: String? = null

    /** The rate the session wants (0: not told yet), the panel's top rate, and since when it is wanted. */
    private var wanted = 0
    private var top = 0
    private var wantedSince = 0L

    /** The rate this process found or put in place while shown; 0 once hidden, put back or after a call that failed. */
    private var placed = 0

    /** When this process last wrote a rate, null: not yet. */
    private var writtenAt: Long? = null

    /** The session wants the panel at [hz], one of its modes, and [topHz] is its top rate. Nothing is written here: [due] tells when. */
    fun want(hz: Int, topHz: Int) {
        top = topHz
        if (hz == wanted) return
        wanted = hz
        wantedSince = clock()
    }

    fun shown(enabled: Boolean, topHz: Int, hz: Int = topHz): String? {
        visible = true
        want(hz, topHz)
        if (!enabled) return restore()
        // Shown again while a change waits: the rate in place is checked, the change keeps its time.
        return lift(if (placed > 0 && due(true) != 0L) placed else wanted)
    }

    fun hidden(): String? {
        visible = false
        return restore()
    }

    fun sessionStarted(enabled: Boolean, topHz: Int, hz: Int = topHz): String? = if (visible) shown(enabled, topHz, hz) else restore()

    fun sessionEnded(): String? = restore()

    fun appStarted(): String? = if (visible) null else restore()

    fun turnedOff(): String? = restore()

    /** After a call failed: puts the value back when it is not wanted raised. */
    fun retry(enabled: Boolean): String? = if (visible && enabled) null else restore()

    /** Whether a value raised here waits to be put back. */
    fun pending(enabled: Boolean): Boolean = !(visible && enabled) && records.load() != null

    /**
     * Milliseconds until the rate wanted may replace the one this process placed (0: now); null while nothing is to
     * change: not shown, the setting off, nothing placed (a call failed: the next start tries again), or it is in place.
     */
    fun due(enabled: Boolean): Long? {
        if (!visible || !enabled || placed <= 0 || wanted <= 0 || wanted == placed) return null
        val steady = wantedSince + STEADY_MS
        val dwell = writtenAt?.let { it + DWELL_MS } ?: steady
        return (maxOf(steady, dwell) - clock()).coerceAtLeast(0L)
    }

    /** Writes the rate wanted once it is [due]. */
    fun settle(enabled: Boolean): String? = if (due(enabled) == 0L) lift(wanted) else null

    private fun lift(hz: Int): String? {
        if (hz <= 0) return null
        placed = 0
        val rate = hz.toString()
        val loaded = records.load()?.let { if (valid(it)) it else { records.clear(); null } }
        val current = secure.read()
        if (loaded != null && loaded.id == armed && holds(loaded, current) && (loaded.confirmed || loaded == attempted)) {
            if (loaded.confirmed && loaded.previous == null && same(current, rate)) return done(hz, null)
            return done(hz, change(loaded, current, rate))
        }
        val marker = secure.marker()
        val record = loaded ?: adopted(marker, current)
        val stale = record?.let { stale(it, current, marker) }
        if (record != null && stale == null) {
            val next = Record(record.original, inPlace(record, current), newId())
            records.save(next)
            secure.arm(next)
            armed = next.id
            attempted = null
            val from = if (loaded == null) "recorded only in the restorer's marker" else "raised before"
            val kept = "$KEY ${next.written} ${from}: kept, the restorer now watches this process"
            return done(hz, if (same(current, rate)) kept else "$kept; ${change(next, current, rate)}")
        }
        val note = stale?.let { ", $it" } ?: ""
        // The top rate is reached by any value at or above it; another rate is wanted as it is.
        val atTop = top in 1..hz
        if (if (atTop) !below(current, hz) else same(current, rate)) {
            if (loaded != null) {
                secure.disarm()
                records.clear()
                forget()
            }
            val what = if (atTop) "reaches the panel's $hz Hz" else "the $hz Hz the session wants"
            return done(hz, "$KEY ${current ?: "unset"}$note: already $what")
        }
        val next = Record(current, rate, newId(), confirmed = false)
        records.save(next)
        attempted = next
        try {
            secure.arm(next)
        } catch (t: Throwable) {
            records.clear()
            attempted = null
            throw t
        }
        armed = next.id
        secure.put(next.written)
        records.save(next.copy(confirmed = true))
        attempted = null
        writtenAt = clock()
        return done(hz, "$KEY ${current ?: "unset"}$note -> ${next.written} while a session is on screen")
    }

    private fun done(hz: Int, line: String?): String? {
        placed = hz
        return line
    }

    /** Puts [rate] in place of the value written for [record], which [current] holds. */
    private fun change(record: Record, current: String?, rate: String): String {
        val from = inPlace(record, current)
        val next = record.copy(written = rate, confirmed = false, previous = from)
        records.save(next)
        attempted = next
        secure.change(next)
        records.save(next.copy(confirmed = true, previous = null))
        attempted = null
        writtenAt = clock()
        return "$KEY $from -> $rate, the rate the session wants now"
    }

    private fun restore(): String? {
        placed = 0
        val record = records.load() ?: return null
        if (!valid(record)) {
            records.clear()
            return "record of $KEY not understood (${record.original} -> ${record.written}): dropped"
        }
        val current = secure.read()
        // This process's own restorer has not acted: it acts only once the process is gone.
        val stale = stale(record, current, if (record.id == armed) null else secure.marker())
        if (stale != null) {
            secure.disarm()
            records.clear()
            forget()
            return "$KEY ${current ?: "unset"}, $stale: left as it is"
        }
        secure.putBack(record.original)
        records.clear()
        forget()
        return "$KEY ${inPlace(record, current)} -> ${record.original ?: "unset"} again"
    }

    private fun forget() {
        armed = null
        attempted = null
    }

    /** Why [record] is not this app's to put back any more, or null while it is. */
    private fun stale(record: Record, current: String?, marker: Marker?): String? {
        val acted = marker?.takeIf { it.id == record.id }?.outcome
        return when {
            !holds(record, current) -> "changed from ${record.written} meanwhile"
            acted == RESTORED -> "put back by the restorer and set to ${inPlace(record, current)} again since"
            acted != null -> "changed and set to ${inPlace(record, current)} again since"
            !record.confirmed && record.previous == null && record != attempted -> "the write of ${record.written} was not confirmed"
            else -> null
        }
    }

    /** The record in a marker no restorer acted on, while the setting holds a value written: the app's own record went with its data. */
    private fun adopted(marker: Marker?, current: String?): Record? {
        if (marker == null || marker.outcome != null) return null
        return Record(marker.original, marker.written, marker.id, previous = marker.previous).takeIf { valid(it) && holds(it, current) }
    }

    companion object {
        const val KEY = "user_refresh_rate"
        const val RESTORED = "restored"
        const val LEFT = "left"
        private const val UNSET = "unset"
        /** How often the restorer looks whether the process is gone, in seconds. */
        const val POLL_S = 3
        /** How long another rate is wanted without a break before it is written. */
        const val STEADY_MS = 2_000L
        /** The least time between two writes of this process. */
        const val DWELL_MS = 5_000L
        private val VALUE = Regex("[0-9]{1,4}(\\.[0-9]{1,4})?")
        private val ID = Regex("[0-9a-z]{1,32}")
        private val PACKAGE = Regex("[A-Za-z0-9._]+")
        private val FILE = Regex("[A-Za-z0-9._/-]+")

        /** Whether [value] is a rate as the setting holds it, so fit for a shell command line. */
        fun isRate(value: String) = VALUE.matches(value)

        /** The output of `settings get`: null when unset; throws on anything but a rate. */
        fun parse(output: String): String? {
            val value = output.trim()
            if (value == "null") return null
            require(VALUE.matches(value)) { "unexpected $KEY '${value.take(40)}'" }
            return value
        }

        /** Whether [current] (null: unset) is below [topHz], so a session would raise it. */
        fun below(current: String?, topHz: Int): Boolean {
            if (topHz <= 0) return false
            val rate = current?.toDoubleOrNull() ?: return current == null
            return rate < topHz - 0.5
        }

        /**
         * The panel's mode for a rate wanted [hz] (0 and below: nothing in particular, the top rate) among [modes]: the one
         * within half a hertz of it, else the slowest faster one, else the top; 0 when no mode is known.
         */
        fun modeFor(hz: Float, modes: Collection<Float>): Int {
            val rates = modes.filter { it > 1f }.map { Math.round(it) }.distinct().sorted()
            val top = rates.lastOrNull() ?: return 0
            if (hz <= 1f) return top
            return rates.firstOrNull { abs(it - hz) <= 0.5f } ?: rates.firstOrNull { it > hz } ?: top
        }

        internal fun same(a: String?, b: String?): Boolean {
            if (a == null || b == null) return a == b
            val x = a.toDoubleOrNull()
            val y = b.toDoubleOrNull()
            return if (x != null && y != null) abs(x - y) < 0.01 else a == b
        }

        /** Whether [current] is a value written for [record]. */
        internal fun holds(record: Record, current: String?) =
            same(current, record.written) || (record.previous != null && same(current, record.previous))

        /** The value written for [record] that [current] is: the one written last unless only the one before matches. */
        private fun inPlace(record: Record, current: String?) =
            if (record.previous != null && !same(current, record.written) && same(current, record.previous)) record.previous else record.written

        internal fun valid(record: Record) =
            VALUE.matches(record.written) && (record.original == null || VALUE.matches(record.original)) && ID.matches(record.id) &&
                (record.previous == null || VALUE.matches(record.previous))

        /** The marker's file for [packageName], in the shell user's /data/local/tmp. */
        fun markerPath(packageName: String): String {
            require(PACKAGE.matches(packageName)) { "unexpected package '$packageName'" }
            return "/data/local/tmp/refresh-lift.$packageName"
        }

        /** The marker's line for [record]. */
        fun markerLine(record: Record) = "${record.id} ${record.original ?: UNSET} ${record.written}" + (record.previous?.let { " $it" } ?: "")

        /** The marker file's contents; null when empty or not understood. */
        fun parseMarker(text: String): Marker? {
            val words = text.trim().split(Regex("\\s+"))
            if (words.size !in 3..4) return null
            val (id, original, written) = words
            val fourth = words.getOrNull(3)
            if (!ID.matches(id) || !VALUE.matches(written) || (original != UNSET && !VALUE.matches(original))) return null
            val outcome = fourth?.takeIf { it == RESTORED || it == LEFT }
            val previous = fourth?.takeIf { VALUE.matches(it) }
            if (fourth != null && outcome == null && previous == null) return null
            return Marker(id, original.takeIf { it != UNSET }, written, outcome, previous)
        }

        /** The shell line that puts [original] back and removes the marker [file]. */
        fun putBackCommand(file: String, original: String?): String {
            require(original == null || isRate(original)) { "not a rate: '$original'" }
            val put = if (original == null) "settings delete secure $KEY" else "settings put secure $KEY $original"
            return "$put && rm -f $file"
        }

        /**
         * The shell line that writes the marker [file] for [record] and starts its restorer, detached so it outlives the
         * connection and process [pid]. Every [pollSeconds] it looks whether [pid] is still there; it stops when the
         * marker is gone or names another restorer. Once [pid] is gone it puts the value before back if the setting
         * still holds a value the marker names as written, and notes [RESTORED] or [LEFT] in the marker.
         */
        fun armCommand(file: String, pid: Int, record: Record, pollSeconds: Int = POLL_S): String {
            require(valid(record) && pid > 0 && pollSeconds > 0 && file.matches(FILE)) { "cannot arm $record" }
            val line = markerLine(record)
            val putBack = if (record.original == null) "settings delete secure $KEY" else "settings put secure $KEY ${record.original}"
            val restorer = "while [ -d /proc/$pid ]; do sleep $pollSeconds; " +
                "read -r a r < $file || exit 0; [ \"\$a\" = ${record.id} ] || exit 0; done; " +
                "read -r a o w p r < $file || exit 0; [ \"\$a\" = ${record.id} ] && [ -z \"\$r\" ] || exit 0; " +
                "case \"\$p\" in $RESTORED|$LEFT) exit 0;; esac; v=\$(settings get secure $KEY); " +
                "if [ \"\$v\" = \"\$w\" ] || { [ -n \"\$p\" ] && [ \"\$v\" = \"\$p\" ]; }; " +
                "then $putBack && echo \$a \$o \$w $RESTORED > $file; else echo \$a \$o \$w $LEFT > $file; fi"
            return "echo $line > $file.tmp && mv -f $file.tmp $file && " +
                "{ setsid sh -c '$restorer' </dev/null >/dev/null 2>&1 & }"
        }

        /**
         * The shell line that puts [record]'s written value in place for the restorer watching the marker [file]: the
         * marker names it and the [Record.previous] value first, then, once the write went through, it alone.
         */
        fun changeCommand(file: String, record: Record): String {
            require(valid(record) && record.previous != null && file.matches(FILE)) { "cannot change to $record" }
            val settled = markerLine(record.copy(previous = null))
            return "echo ${markerLine(record)} > $file.tmp && mv -f $file.tmp $file && " +
                "settings put secure $KEY ${record.written} && echo $settled > $file.tmp && mv -f $file.tmp $file"
        }
    }
}
