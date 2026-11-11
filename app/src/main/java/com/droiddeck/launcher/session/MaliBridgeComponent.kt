package com.droiddeck.launcher.session

import android.content.Context
import android.util.Log
import com.droiddeck.launcher.core.HostProcess
import com.droiddeck.launcher.core.SessionPart
import java.io.File

/**
 * [mali-g2] MaliBridge (github.com/ASEbrahim/MaliBridge, GPL-3.0) as an alternative to Vortek, for
 * comparing the two on one device. Off unless droiddeck-env has BL_VK_BRIDGE=malibridge.
 *
 * Its server, lib/arm64-v8a/libvkbridge_server.so, is a bionic executable like Vortek's: it loads
 * the system Vulkan loader and runs the calls the guest ICD (libvulkan_droidbridge.so) sends over a
 * unix socket. The binaries are built from the external/MaliBridge submodule, as upstream's own Mali bridge
 * uses them (app/build.gradle builds the server, tools/components/recipes/vkbridge.sh the guest ICD).
 * The socket is filesDir/.wayland-rt/vkbridge/S, which the guest sees as [GUEST_SOCKET]. The server
 * runs a self-test before it listens, so start() waits for the socket.
 */
class MaliBridgeComponent(
    private val socket: File,
    private val logFile: File,
    private val extraEnv: List<String>,
) : SessionPart() {
    @Volatile private var pid = -1
    @Volatile private var exited = false

    override fun start() {
        stop()
        val binary = binary(app())
        if (!binary.isFile) {
            Log.w(TAG, "not starting: no $binary")
            return
        }
        socket.parentFile?.mkdirs()
        socket.delete()
        exited = false
        // Pipeline caches kept across sessions: the driver recompiles everything in a new process otherwise.
        val cacheDir = File(app().cacheDir, "vkbridge-pipelines").apply { mkdirs() }
        val command = listOf(binary.absolutePath, "--socket", socket.absolutePath, "--log", logFile.absolutePath,
            "--cache-dir", cacheDir.absolutePath)
            .joinToString(" ") { it.replace("\\", "\\\\").replace(" ", "\\ ") }
        val env = (listOf("HOME=" + app().filesDir) + extraEnv).toTypedArray()
        pid = HostProcess.start(command, env, app().filesDir, { status ->
            exited = true
            Log.w(TAG, "server exited: $status")
            runCatching { logFile.appendText("== vkbridge server exited: $status\n") }
        }) { line -> Log.i(TAG, line) }
        val deadline = System.currentTimeMillis() + 15_000L
        while (!socket.exists() && !exited && System.currentTimeMillis() < deadline) {
            try { Thread.sleep(50L) } catch (e: InterruptedException) { Thread.currentThread().interrupt(); break }
        }
        Log.i(TAG, if (socket.exists()) "server ready (pid $pid) at $socket" else "server NOT ready (pid $pid, exited=$exited); see $logFile")
    }

    override fun stop() {
        if (pid > 1) android.os.Process.killProcess(pid)
        pid = -1
    }

    companion object {
        private const val TAG = "MaliBridge"
        const val BINARY = "libvkbridge_server.so"
        const val SWITCH = "BL_VK_BRIDGE=malibridge"
        /** filesDir/.wayland-rt is bound to /run/droiddeck in the guest. */
        const val GUEST_SOCKET = "/run/droiddeck/vkbridge/S"
        const val GUEST_ICD = "/usr/local/share/vkbridge/droidbridge_icd.json"

        fun binary(context: Context): File = File(context.applicationInfo.nativeLibraryDir, BINARY)

        fun available(context: Context): Boolean = binary(context).isFile
    }
}
