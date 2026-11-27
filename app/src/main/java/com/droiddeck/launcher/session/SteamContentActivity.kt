package com.droiddeck.launcher.session

import java.io.File
import java.io.RandomAccessFile

/**
 * Steps of the Steam client's library work that should not be cut by asking it to exit, from content_log.txt:
 * an app, workshop or shader update in `Committing` or `Running Script` (an install script; `AppID <id> App
 * update changed : Running Update,Running Script,` until another state), and uninstalls (`state changed :
 * ...Uninstalling,` until a state without it), since the client last started (`Client version:`). Downloads,
 * validation and other update steps resume when the client starts again and are not listed.
 * [poll] reads only what was appended since the previous call.
 */
internal class SteamContentActivity(private val firstReadBytes: Long = FIRST_READ_BYTES) {
    private val updates = LinkedHashMap<String, String>()
    private val uninstalls = LinkedHashSet<String>()
    private var log: File? = null
    private var identity: Any? = null
    private var offset = 0L

    /** What is in progress, as `App 1473480 (Running Script)`, `Workshop 4000 (Committing)`, `uninstall 730`. */
    fun busy(): List<String> = updates.map { (key, step) -> "$key ($step)" } + uninstalls.map { "uninstall $it" }

    fun feed(line: String) {
        if ("Client version:" in line) {
            updates.clear()
            uninstalls.clear()
            return
        }
        SteamDownloadMonitor.CONTENT_UPDATE.find(line)?.let { match ->
            val key = "${match.groupValues[2]} ${match.groupValues[1]}"
            val state = match.groupValues[3]
            val step = UNCUT_STEPS.firstOrNull { it in state }
            if (step != null && "Running Update" in state && "Stopping" !in state) updates[key] = step else updates -= key
            return
        }
        STATE_CHANGED.find(line)?.let { match ->
            if ("Uninstalling" in match.groupValues[2]) uninstalls += match.groupValues[1]
            else uninstalls -= match.groupValues[1]
        }
    }

    /** [poll] on the content_log.txt of the client that runs ([contentLog]). */
    fun poll(runtimeRoot: File, client: String?): List<String> = poll(contentLog(runtimeRoot, client))

    fun poll(file: File?): List<String> {
        if (file == null || !file.isFile) {
            forget(null)
            return emptyList()
        }
        val key = fileKey(file)
        val size = file.length()
        if (file != log || key != identity || size < offset) {
            forget(file)
            identity = key
            offset = (size - firstReadBytes).coerceAtLeast(0L)
            if (offset > 0L) offset = nextLineStart(file, offset)
        }
        if (size > offset) runCatching { read(file, size) }
        return busy()
    }

    private fun read(file: File, size: Long) {
        RandomAccessFile(file, "r").use { raf ->
            raf.seek(offset)
            val bytes = ByteArray((size - offset).toInt())
            raf.readFully(bytes)
            val end = bytes.lastIndexOf('\n'.code.toByte())
            if (end < 0) return
            String(bytes, 0, end, Charsets.ISO_8859_1).lineSequence().forEach { feed(it.trimEnd('\r')) }
            offset += end + 1
        }
    }

    private fun nextLineStart(file: File, from: Long): Long = runCatching {
        RandomAccessFile(file, "r").use { raf ->
            raf.seek(from - 1)
            while (raf.filePointer < raf.length()) if (raf.read() == '\n'.code) return@use raf.filePointer
            raf.length()
        }
    }.getOrDefault(from)

    private fun forget(file: File?) {
        updates.clear()
        uninstalls.clear()
        log = file
        identity = null
        offset = 0L
    }

    private fun fileKey(file: File): Any? = runCatching {
        java.nio.file.Files.readAttributes(file.toPath(), java.nio.file.attribute.BasicFileAttributes::class.java).fileKey()
    }.getOrNull()

    companion object {
        /** content_log.txt of the client that runs ([client] is `x86_64` for the x86-64 client). */
        fun contentLog(runtimeRoot: File, client: String?): File? {
            val homes = client?.let { listOf(if (it == "x86_64") SteamDownloadMonitor.STEAM_HOMES[1] else SteamDownloadMonitor.STEAM_HOMES[0]) }
                ?: SteamDownloadMonitor.STEAM_HOMES
            return homes.map { File(runtimeRoot, "$it/logs/content_log.txt") }.filter { it.isFile }.maxByOrNull { it.lastModified() }
        }

        private val UNCUT_STEPS = listOf("Running Script", "Committing")
        private val STATE_CHANGED = Regex("""AppID (\d+) state changed : (.*)""")
        private const val FIRST_READ_BYTES = 4L * 1024L * 1024L
    }
}
