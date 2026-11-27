package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.play.LaunchCmdline
import java.io.File

object SteamGameProcess {
    /** A game Steam started; not its install script, the redistributable installers that runs, or another tool verb. */
    fun matches(cmdline: ByteArray): Boolean {
        val args = String(cmdline).split('\u0000')
        if (LaunchCmdline.steamHelper(args)) return false
        for (i in args.indices) {
            if (args[i] == "waitforexitandrun") return true
            if (args[i] == "SteamLaunch" && args.getOrNull(i + 1)?.startsWith("AppId=") == true) return true
        }
        return false
    }

    fun running(proc: File = File("/proc")): Boolean = proc.listFiles()?.any { p ->
        p.name.all(Char::isDigit) && runCatching { File(p, "cmdline").readBytes() }.getOrNull()?.let(::matches) == true
    } == true
}
