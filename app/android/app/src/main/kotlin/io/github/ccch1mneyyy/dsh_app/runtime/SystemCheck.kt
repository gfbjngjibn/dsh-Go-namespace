package io.github.ccch1mneyyy.dsh_app.runtime

import android.content.Context
import android.os.Process as AndroidProcess
import android.system.Os

/**
 * What [SystemContainer] needs, probed for real rather than read from config (as
 * Droidspaces' check does): root through su, each kernel namespace, KernelSU's
 * no-new-privs, a delegable cgroup2, the launcher and an image, and a trial that su
 * refuses this app's uid under no-new-privs. The kernel options it does without are
 * warnings. Blocking: call off the main thread.
 */
object SystemCheck {
    /**
     * One requirement. [detail] is a stable code for the UI, or the launcher's line for [TRIAL].
     * A [warn] item that is not ok costs a feature, not the launch.
     */
    data class Item(val key: String, val ok: Boolean, val detail: String? = null, val warn: Boolean = false) {
        fun toMap(): Map<String, Any?> = mapOf("key" to key, "ok" to ok, "detail" to detail, "warn" to warn)
    }

    const val ROOT = "root"
    const val NO_NEW_PRIVS = "noNewPrivs"
    const val TRIAL = "trial"
    const val LAUNCHER = "launcher"
    const val CGROUP2 = "cgroup2"
    const val IMAGE = "image"
    /** Kernel options the system container does without (no resource limits, no nftables...). */
    val KERNEL_OPTIONS = listOf("CGROUP_PIDS", "NF_TABLES", "BRIDGE_NETFILTER", "AUTOFS_FS", "CHECKPOINT_RESTORE")
    /** toybox `unshare` flag for each namespace the launcher creates. */
    val NAMESPACES = linkedMapOf("mnt" to "m", "pid" to "p", "ipc" to "i", "uts" to "u", "user" to "U", "cgroup" to "C")

    // Detail codes.
    const val NOT_GRANTED = RootShell.NOT_GRANTED
    const val NEEDS_ROOT = "needsRoot"
    const val UNSUPPORTED = "unsupported"
    const val SKIPPED = "skipped"
    const val ESCALATED = "escalated"
    const val TIMED_OUT = RootShell.TIMED_OUT
    const val MISSING = "missing"
    const val OFF = "off"

    /** A throwaway cgroup, so the check leaves nothing named dsh-go behind. */
    private val PROBE = "echo dsh-ns-uid=$(id -u); " +
        NAMESPACES.values.joinToString(" ") { "unshare -$it true >/dev/null 2>&1 && echo dsh-ns-$it=ok;" } +
        " d=/sys/fs/cgroup/dsh-check-$$; mkdir \$d 2>/dev/null && { test -e \$d/cgroup.kill && echo dsh-cg2=ok; rmdir \$d; }; " +
        "zcat /proc/config.gz 2>/dev/null | grep -E '^(# )?CONFIG_(" + KERNEL_OPTIONS.joinToString("|") + ")[= ]'"

    fun run(context: Context): List<Item> {
        val items = ArrayList<Item>()
        val probe = su(listOf("-c", PROBE))
        val root = probe.lines.contains("dsh-ns-uid=0")
        items += Item(ROOT, root, if (root) null else probe.failure ?: NOT_GRANTED)
        for ((name, flag) in NAMESPACES) {
            val ok = root && probe.lines.contains("dsh-ns-$flag=ok")
            items += Item(name, ok, if (ok) null else if (root) UNSUPPORTED else NEEDS_ROOT)
        }
        val nnp = root && su(listOf("-W", "--ksu-no-new-privs", "-c", "echo dsh-ns-nnp=ok")).lines.contains("dsh-ns-nnp=ok")
        items += Item(NO_NEW_PRIVS, nnp, if (nnp) null else if (root) UNSUPPORTED else NEEDS_ROOT)
        val cgroup = root && probe.lines.contains("dsh-cg2=ok")
        items += Item(CGROUP2, cgroup, if (cgroup) null else if (root) UNSUPPORTED else NEEDS_ROOT)
        val launcher = SystemContainer.layout(context).launcher
        items += Item(LAUNCHER, launcher.canExecute(), if (launcher.canExecute()) null else MISSING)
        val image = SystemContainer.bundled(context)
        items += Item(IMAGE, image != null, image?.version ?: MISSING)
        items += if (items.any { !it.ok }) Item(TRIAL, false, SKIPPED) else trial(launcher.path)
        items += kernelOptions(probe.lines, root)
        return items
    }

    /**
     * Under `su --ksu-no-new-privs`, as the container is started, `dshns trial` becomes this
     * app's uid (which KernelSU grants root) and runs `/system/bin/su -c 'id -u'`: su still
     * runs when refused, so the verdict is the uid it ran as, never its exit code.
     */
    private fun trial(launcher: String): Item {
        val command = "${DirectLaunch.quote(launcher)} trial --uid ${AndroidProcess.myUid()} --gid ${Os.getgid()}"
        val run = RootShell.run(listOf("/system/bin/su", "-W", "--ksu-no-new-privs", "-c", command), timeoutMs = 20_000)
        return trialResult(run.lines, run.failure)
    }

    /** Pure verdict over `dshns trial`'s output: `su-uid N`, or `su-uid -` when su did not run. */
    fun trialResult(lines: List<String>, failure: String? = null): Item {
        val su = lines.firstOrNull { it.startsWith("su-uid ") }?.removePrefix("su-uid ")
        return when {
            su == "0" -> Item(TRIAL, false, ESCALATED)
            su != null -> Item(TRIAL, true)
            else -> Item(TRIAL, false, lines.lastOrNull { it.startsWith("[dshns] ") } ?: failure ?: lines.lastOrNull() ?: UNSUPPORTED)
        }
    }

    /** Warnings for the kernel options in `zcat /proc/config.gz` lines: [OFF], or [UNSUPPORTED] when unreadable. */
    fun kernelOptions(lines: List<String>, root: Boolean): List<Item> = KERNEL_OPTIONS.map { name ->
        val set = lines.any { it.startsWith("CONFIG_$name=") && !it.endsWith("=n") }
        val known = set || lines.any { it == "# CONFIG_$name is not set" || it == "CONFIG_$name=n" }
        Item("kconfig:$name", set, if (set) null else if (!root) NEEDS_ROOT else if (known) OFF else UNSUPPORTED, warn = true)
    }

    private fun su(args: List<String>): RootShell.Output = RootShell.run(listOf("/system/bin/su") + args, timeoutMs = 10_000)
}
