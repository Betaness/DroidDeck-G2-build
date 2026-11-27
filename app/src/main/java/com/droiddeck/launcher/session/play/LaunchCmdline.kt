package com.droiddeck.launcher.session.play

/**
 * [mali-g2] A Steam game launch, read from a process's argv.
 *
 * Steam starts every game as `steam-launch-wrapper -- reaper SteamLaunch AppId=<id> -- <tool> <verb> <exe> ...`
 * (gameprocess_log.txt shows the line). The wrapper and the reaper both carry it, so the play lasts while
 * either lives. With Proton the tool is the compatibility tool and the verb `waitforexitandrun`; Steam
 * also runs install scripts and the evaluator through the same tool with the `run` verb, which are not plays.
 * A native game has no verb: its binary follows the second `--`.
 *
 * An install script (console_log.txt: `Running install script evaluator for AppID <id>`) is
 * `reaper SteamLaunch AppId=<id> Install=1 -- <tool> run <Steam>/legacycompat/iscriptevaluator.exe ...`;
 * the redistributable installers it runs are its children. content_log.txt shows it as the app's update in
 * `Running Script`, and gameprocess_log.txt tracks none of its processes.
 */
object LaunchCmdline {
    class Launch(val appid: Long, /** Relative to steamapps/common when it is there. */ val exe: String?) {
        val exeName: String? get() = exe?.substringAfterLast('/')?.takeIf { it.isNotEmpty() }
    }

    private val VERBS = setOf("waitforexitandrun", "run", "waitforexit", "getcompatpath", "getnativepath", "runinprefix", "destroyprefix")
    private const val EVALUATOR = "iscriptevaluator.exe"

    fun parse(argv: List<String>): Launch? {
        val launch = argv.indexOf("SteamLaunch")
        if (launch < 0 || steamHelper(argv)) return null
        val appid = argv.getOrNull(launch + 1)?.takeIf { it.startsWith("AppId=") }
            ?.removePrefix("AppId=")?.toLongOrNull()?.takeIf { it > 0 } ?: return null
        val sep = argv.subList(launch, argv.size).indexOf("--").takeIf { it >= 0 }?.plus(launch) ?: return Launch(appid, null)
        val rest = argv.subList(sep + 1, argv.size)
        val verbAt = rest.indexOfFirst { it in VERBS }
        val exe = when {
            verbAt >= 0 && rest[verbAt] != "waitforexitandrun" -> return null
            verbAt >= 0 -> rest.drop(verbAt + 1).firstOrNull { it.isNotBlank() }
            else -> rest.firstOrNull { it.isNotBlank() }
        }
        return Launch(appid, exe?.let(::relative))
    }

    /**
     * Whether [argv] is something Steam runs for a game that is not the game: the install-script evaluator
     * (or a process started with it), a launch marked `Install=`, or a compatibility tool verb other than
     * `waitforexitandrun` (`run`, `getcompatpath`, ...; also as pressure-vessel's `--verb=`).
     */
    fun steamHelper(argv: List<String>): Boolean {
        if (argv.any { it.replace('\\', '/').substringAfterLast('/').equals(EVALUATOR, ignoreCase = true) }) return true
        val launch = argv.indexOf("SteamLaunch")
        if (launch < 0) return false
        val sep = (launch + 1 until argv.size).firstOrNull { argv[it] == "--" } ?: argv.size
        if (argv.subList(launch + 1, sep).any { it.startsWith("Install=") && it != "Install=0" }) return true
        val tool = argv.subList(minOf(sep + 1, argv.size), argv.size)
        val verb = tool.firstOrNull { it.startsWith("--verb=") }?.removePrefix("--verb=")
            ?: tool.getOrNull(1)?.takeIf { it in VERBS }
        return verb != null && verb != "waitforexitandrun"
    }

    fun relative(path: String): String {
        val p = path.trim().trim('\'', '"').replace('\\', '/')
        val common = p.indexOf("/steamapps/common/")
        if (common >= 0) return p.substring(common + "/steamapps/common/".length)
        val parts = p.split('/').filter { it.isNotEmpty() }
        return parts.takeLast(2).joinToString("/")
    }

    /** Whether a process whose argv[0] is [argv0] runs [exeName] (wine shows the Windows path). */
    fun runs(argv0: String, exeName: String): Boolean {
        if (exeName.isEmpty()) return false
        val base = argv0.replace('\\', '/').substringAfterLast('/')
        return base.equals(exeName, ignoreCase = true)
    }
}
