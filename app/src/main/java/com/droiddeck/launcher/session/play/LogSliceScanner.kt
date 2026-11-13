package com.droiddeck.launcher.session.play

import java.io.File
import java.io.RandomAccessFile

/**
 * [mali-g2] Reads the part of each log a play wrote, after the play: what went wrong, and a few facts
 * (swapchain size, renderer, DXVK version, Steam's exit code). Never on the render path.
 *
 * Each [Source] is read from its offset (resynced to the next line) up to its end mark, at most [maxBytes]
 * of it: when there is more, the first [headBytes] (DXVK's version, the device, the first swapchain) and
 * the rest from the end. Line by line with plain `contains` checks before any regex. Kept: per kind of
 * problem the count, the labels it was seen in, the first [perKind] lines ([total] lines in all, each cut
 * to [lineMax]) and when it was first seen, for lines that carry a time.
 *
 * [uptimeToWallMs] turns the X11 layers' `(t=<CLOCK_MONOTONIC s>)` stamps into wall time.
 */
class LogSliceScanner(
    private val maxBytes: Long = 16L * 1024 * 1024,
    private val headBytes: Long = 2L * 1024 * 1024,
    private val perKind: Int = 3,
    private val total: Int = 20,
    private val lineMax: Int = 240,
    private val uptimeToWallMs: Long? = null,
    private val nowMs: Long = System.currentTimeMillis(),
) {
    /**
     * [until] is where the slice ends (the length when the play ended; -1: the current end). [game] marks the
     * game's own logs, as opposed to the session's, which Steam's client and other games write to as well.
     */
    class Source(val label: String, val file: File, val from: Long = 0L, val until: Long = -1L, val game: Boolean = true)

    class Kind(var n: Int = 0, val first: MutableList<String> = ArrayList()) {
        val labels = LinkedHashMap<String, Int>()
        /** Wall time of the first line of this kind that carried a time. */
        var firstAtMs: Long? = null
    }

    class Findings {
        val errors = LinkedHashMap<String, Kind>()
        var kept = 0
        val screenFrames = ArrayList<Int>()
        val gpuFrames = ArrayList<Int>()
        val zeroCopyFrames = ArrayList<Int>()
        val x11Fps = ArrayList<Double>()
        private val swapchainSet = LinkedHashSet<String>()
        var dxvk: String? = null
        var renderer: String? = null
        var gpuUsed: String? = null
        var steamWrapperPid: Int? = null
        var steamExitCode: Int? = null
        var steamEndedAt: String? = null
        var steers = 0
        var vortekExit: Int? = null
        /** DXVK's or vkd3d's "Found device", for games that log no renderer of their own. */
        var device: String? = null
        /** llvmpipe / softpipe seen: drawn on the CPU. */
        var software = false
        val sources = ArrayList<String>()
        /** Labels of the game's own logs (see [Source.game]). */
        val gameLabels = LinkedHashSet<String>()
        /**
         * D3D11 stream output, from the GPU paths' "xfb stats" lines in the game's logs
         * (mali-g2/vortek/src/xfb_stats.h): the last line of each process, by pid.
         */
        val xfb = LinkedHashMap<Int, XfbLine>()

        /** Summed over the processes; null when no line was seen (none used stream output or geometry shaders). */
        fun xfbTotals(): Map<String, Any?>? {
            if (xfb.isEmpty()) return null
            val begins = xfb.values.sumOf { it.begins }
            val seconds = xfb.values.maxOf { it.seconds }
            return linkedMapOf(
                "pipelines" to xfb.values.sumOf { it.pipelines },
                "gs" to xfb.values.sumOf { it.gs },
                "begins" to begins,
                "drawAuto" to xfb.values.sumOf { it.drawAuto },
                // Over the longest-running process's seconds: the other processes' counts fall in them too.
                "beginsPerMin" to if (seconds > 0) PlayAccumulator.r1(begins * 60.0 / seconds) else null,
            )
        }

        fun swapchain(size: String) { if (swapchainSet.size < 32) swapchainSet.add(size) }

        fun swapchains(): List<String> = swapchainSet.sortedByDescending { area(it) }

        fun has(kind: String) = (errors[kind]?.n ?: 0) > 0

        /** Seen in one of the game's own logs. */
        fun inGame(kind: String) = errors[kind]?.labels?.keys?.any { it in gameLabels } ?: false

        fun firstLine(kind: String): String? = errors[kind]?.first?.firstOrNull()

        fun firstGameLine(kind: String): String? =
            errors[kind]?.first?.firstOrNull { line -> gameLabels.any { line.startsWith("$it: ") } }
                ?: errors[kind]?.takeIf { inGame(kind) }?.first?.firstOrNull()

        private fun area(s: String): Long = s.split('x').mapNotNull { it.toLongOrNull() }.takeIf { it.size == 2 }?.let { it[0] * it[1] } ?: 0L
    }

    class XfbLine(val pipelines: Long, val gs: Long, val begins: Long, val drawAuto: Long, val seconds: Long)

    private class Pattern(val kind: String, val needle: String)

    fun scan(sources: List<Source>, appid: Long? = null): Findings = Findings().also { scanInto(it, sources, appid) }

    fun scanInto(f: Findings, sources: List<Source>, appid: Long? = null) {
        for (s in sources) {
            if (!s.file.isFile) continue
            f.sources.add(s.label)
            if (s.game) f.gameLabels.add(s.label)
            try {
                read(s) { line -> look(line, s.label, appid, f) }
            } catch (_: Exception) {
            }
        }
    }

    fun note(f: Findings, kind: String, line: String, label: String) = keep(f, kind, line, label)

    private fun read(s: Source, each: (String) -> Unit) {
        RandomAccessFile(s.file, "r").use { raf ->
            val length = raf.length().let { if (s.until >= 0) minOf(it, s.until) else it }
            val from = s.from.coerceIn(0L, length)
            if (length - from <= maxBytes) {
                range(raf, from, length, each)
            } else {
                range(raf, from, from + headBytes, each)
                range(raf, length - (maxBytes - headBytes), length, each)
            }
        }
    }

    /** The whole lines that start in [from, end): a line cut by [from] is skipped, one cut by [end] read to its end. */
    private fun range(raf: RandomAccessFile, from: Long, end: Long, each: (String) -> Unit) {
        var pos = from
        if (pos > 0) {
            raf.seek(pos - 1)
            if (raf.read() != '\n'.code) {
                val skip = java.io.BufferedInputStream(java.io.FileInputStream(raf.fd), 8 * 1024)
                raf.seek(pos)
                var n = 0L
                while (true) { val b = skip.read(); if (b < 0) return; n++; if (b == '\n'.code) break }
                pos += n
            }
        }
        raf.seek(pos)
        val reader = java.io.BufferedReader(java.io.InputStreamReader(java.io.FileInputStream(raf.fd), Charsets.UTF_8), 64 * 1024)
        var left = end - pos
        while (left > 0) {
            val line = reader.readLine() ?: break
            left -= line.toByteArray(Charsets.UTF_8).size + 1
            each(if (line.length > 4096) line.substring(0, 4096) else line)
        }
    }

    private fun keep(f: Findings, kind: String, line: String, label: String) {
        val k = f.errors.getOrPut(kind) { Kind() }
        k.n++
        k.labels[label] = (k.labels[label] ?: 0) + 1
        if (k.firstAtMs == null) k.firstAtMs = stamp(line)
        if (k.first.size < perKind && f.kept < total) {
            val text = "$label: ${line.trim()}"
            k.first.add(if (text.length > lineMax) text.substring(0, lineMax - 1) + "…" else text)
            f.kept++
        }
    }

    private fun look(line: String, label: String, appid: Long?, f: Findings) {
        if (facts(line, label, appid, f)) return
        for (p in PATTERNS) {
            if (line.contains(p.needle)) {
                keep(f, p.kind, line, label)
                return
            }
        }
        val hidden = line.contains("now hidden, frames paced")
        if (hidden || line.contains("now through X11")) {
            // Small helper windows are hidden all the time; a game-sized one is worth noting.
            val size = WINDOW_SIZE.find(line)?.destructured?.let { (w, h) -> w.toLong() * h.toLong() } ?: 0L
            if (size >= 640L * 360L) keep(f, if (hidden) "hidden" else "x11_copy", line, label)
        }
    }

    /** Wall time of a line: logcat's "MM-DD HH:MM:SS.mmm", Steam's "[YYYY-MM-DD HH:MM:SS]", or "(t=<monotonic s>)". */
    fun stamp(line: String): Long? {
        try {
            LOGCAT_STAMP.find(line)?.let { m ->
                val cal = java.util.Calendar.getInstance()
                cal.timeInMillis = nowMs
                val year = cal.get(java.util.Calendar.YEAR)
                val fmt = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", java.util.Locale.US)
                var t = fmt.parse("$year-${m.groupValues[1]}")?.time ?: return null
                // Read in January about December.
                if (t > nowMs + 86_400_000L) t = fmt.parse("${year - 1}-${m.groupValues[1]}")?.time ?: return null
                return t
            }
            STAMP.find(line)?.let { m ->
                return java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.US).parse(m.groupValues[1])?.time
            }
            val offset = uptimeToWallMs ?: return null
            MONO_STAMP.find(line)?.let { m -> return (m.groupValues[1].toDouble() * 1000).toLong() + offset }
        } catch (_: Exception) {
        }
        return null
    }

    private fun facts(line: String, label: String, appid: Long?, f: Findings): Boolean {
        when {
            line.contains("[stats] last 10 s:") -> {
                STATS_SCREEN.find(line)?.let { f.screenFrames.add(it.groupValues[1].toInt()) }
                STATS_GPU.find(line)?.let { f.gpuFrames.add(it.groupValues[1].toInt()) }
                STATS_ZERO.find(line)?.let { f.zeroCopyFrames.add(it.groupValues[1].toInt()) }
                return true
            }
            line.contains("-x11: ") && line.contains(" fps") -> {
                X11_FPS.find(line)?.let { f.x11Fps.add(it.groupValues[1].toDouble()); return true }
            }
            line.contains("swapchain") -> {
                (ZERO_COPY_SWAPCHAIN.find(line) ?: CLIENT_SWAPCHAIN.find(line))?.let {
                    val (w, h) = it.destructured
                    if (w.toInt() >= 320 && h.toInt() >= 200) f.swapchain("${w}x$h")
                    return true
                }
            }
            line.contains("info:  DXVK: v") -> { f.dxvk = line.substringAfter("DXVK: ").trim().take(60); return true }
            line.contains("Renderer:") && f.renderer == null -> {
                f.renderer = line.substringAfter("Renderer:").trim().take(80)
                if (SOFTWARE.any { f.renderer!!.contains(it) }) f.software = true
                return true
            }
            line.contains("droiddeck-game-env:") && line.contains("Vulkan in-process (libhybris)") -> { f.gpuUsed = "hybris"; return true }
            line.contains("droiddeck-game-env:") && line.contains("in-process GPU asked for, but") -> { f.gpuUsed = "vortek"; return true }
            line.contains("cluster steering: moved") -> { f.steers++; return true }
            line.contains(": xfb stats: pid ") -> {
                // Steam's own processes write to the session's logs, and zink has transform feedback too.
                if (label !in f.gameLabels) return true
                XFB_STATS.find(line)?.destructured?.let { (pid, p, g, b, a, sec) ->
                    f.xfb[pid.toInt()] = XfbLine(p.toLong(), g.toLong(), b.toLong(), a.toLong(), sec.toLong())
                }
                return true
            }
            line.contains("Found device: ") && f.device == null && line.contains("info:") && label in f.gameLabels -> {
                f.device = line.substringAfter("Found device: ").trim().take(80)
                if (SOFTWARE.any { f.device!!.contains(it) }) f.software = true
                return true
            }
            line.contains("vortek-server exited: ") -> {
                f.vortekExit = line.substringAfter("vortek-server exited: ").trim().substringBefore(' ').toIntOrNull()
                keep(f, "vortek", line, label)
                return true
            }
            appid != null && line.contains("AppID $appid ") -> {
                if (f.steamWrapperPid == null) TRACKED.find(line)?.let {
                    if (line.contains("tracked process \"")) f.steamWrapperPid = it.groupValues[1].toInt()
                }
                val pid = f.steamWrapperPid
                if (pid != null && line.contains("no longer tracking PID $pid,")) {
                    f.steamExitCode = EXIT_CODE.find(line)?.groupValues?.get(1)?.toIntOrNull()
                    f.steamEndedAt = STAMP.find(line)?.groupValues?.get(1)
                }
                return true
            }
        }
        return false
    }

    companion object {
        private val PATTERNS = listOf(
            Pattern("crash", "Crash!!!"),
            Pattern("crash", "Unhandled exception"),
            Pattern("crash", "page fault on"),
            Pattern("crash", "EXCEPTION_ACCESS_VIOLATION"),
            Pattern("crash", "Fatal signal"),
            Pattern("device_lost", "VK_ERROR_DEVICE_LOST"),
            Pattern("device_lost", "DEVICE_LOST"),
            Pattern("device_lost", "Device lost"),
            Pattern("unsupported", "Skipping: Device does not support"),
            Pattern("unsupported", "No adapters found"),
            Pattern("unsupported", "failed to create factory"),
            Pattern("oom", "Scudo ERROR"),
            Pattern("oom", "Out of memory"),
            Pattern("oom", "OUT_OF_HOST_MEMORY"),
            Pattern("oom", "OUT_OF_DEVICE_MEMORY"),
            Pattern("oom", "bad_alloc"),
            Pattern("compiler", "cmpbe_v2_compile"),
            Pattern("no_sempool", "no semaphore pool in this pack"),
            Pattern("no_sempool", "droiddeck-esync: no semaphore pool ("),
            Pattern("no_sempool", ", no semaphore pool)"),
            Pattern("vulkan", "err:vulkan"),
            Pattern("vulkan", "DxvkError"),
            Pattern("vulkan", "err:   "),
            Pattern("hidden", "zero-copy refused"),
            Pattern("module", "err:module"),
        )
        private val WINDOW_SIZE = Regex("""window 0x[0-9a-f]+ (\d+)x(\d+)""")
        private val STATS_SCREEN = Regex("""(\d+) frames on screen""")
        private val STATS_GPU = Regex("""(\d+) GPU frames from games""")
        private val STATS_ZERO = Regex("""(\d+) Vortek zero-copy frames""")
        private val X11_FPS = Regex("""-x11: \d+ frames, ([0-9.]+) fps""")
        private val ZERO_COPY_SWAPCHAIN = Regex("""zero-copy swapchain \d+ of presenter \d+: (\d+)x(\d+)""")
        private val CLIENT_SWAPCHAIN = Regex("""(?:vortek-x11|hybris-wsi): swapchain (\d+)x(\d+) (?:for window|format)""")
        private val XFB_STATS = Regex("""xfb stats: pid (\d+), pipelines (\d+), gs (\d+), begins (\d+), drawauto (\d+), seconds (\d+)""")
        private val TRACKED = Regex("""adding PID (\d+) as a tracked process""")
        private val EXIT_CODE = Regex("""exit code (-?\d+)""")
        private val STAMP = Regex("""^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})]""")
        private val LOGCAT_STAMP = Regex("""^(\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\s""")
        private val MONO_STAMP = Regex("""\(t=(\d+\.\d+)\)\s*$""")
        private val SOFTWARE = listOf("llvmpipe", "softpipe", "SwiftShader")
    }
}
