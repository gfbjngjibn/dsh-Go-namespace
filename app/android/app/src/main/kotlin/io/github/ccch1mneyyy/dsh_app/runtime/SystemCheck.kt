package io.github.ccch1mneyyy.dsh_app.runtime

import android.content.Context
import android.os.Process as AndroidProcess
import android.system.Os

/**
 * What [SystemContainer] needs, probed for real rather than read from config (as
 * Droidspaces' check does): root through su, each kernel namespace, KernelSU's
 * no-new-privs, a delegable cgroup2, the launcher and an image, and a trial that su
 * refuses this app's uid under no-new-privs, and the kernel options of a kernel patched
 * for containers (stock kernels lack them and are not supported). Blocking: call off
 * the main thread.
 */
object SystemCheck {
    /**
     * One requirement. [detail] is a stable code for the UI, or the launcher's line for [TRIAL];
     * [missing] names the options of a kernel item that are off.
     */
    data class Item(val key: String, val ok: Boolean, val detail: String? = null, val missing: List<String> = emptyList()) {
        fun toMap(): Map<String, Any?> = mapOf("key" to key, "ok" to ok, "detail" to detail, "missing" to missing)
    }

    const val ROOT = "root"
    const val NO_NEW_PRIVS = "noNewPrivs"
    const val TRIAL = "trial"
    const val LAUNCHER = "launcher"
    const val CGROUP2 = "cgroup2"
    const val IMAGE = "image"
    /**
     * The kernel options the system container requires, one item each with the options that
     * come with it: the pids controller dshns caps the container with, nftables (iptables-nft,
     * netavark), IPv6 NAT, bridge netfilter for Docker's isolation, and what else the patched
     * kernel carries. Not CGROUP_DEVICE nor IPVLAN_L3S: both would change the KMI, so it goes without.
     */
    val KERNEL_OPTIONS = linkedMapOf(
        "CGROUP_PIDS" to listOf("CGROUP_PIDS"),
        "NF_TABLES" to listOf(
            "NF_TABLES", "NF_TABLES_INET", "NF_TABLES_IPV4", "NF_TABLES_IPV6", "NFT_CT", "NFT_NAT", "NFT_MASQ",
            "NFT_REDIR", "NFT_REJECT", "NFT_COMPAT", "NFT_FIB_INET", "NFT_FIB_IPV4", "NFT_FIB_IPV6",
        ),
        "IP6_NF_NAT" to listOf("IP6_NF_NAT", "IP6_NF_TARGET_MASQUERADE"),
        "BRIDGE_NETFILTER" to listOf("BRIDGE_NETFILTER"),
        "IP_SET" to listOf(
            "IP_SET", "IP_SET_HASH_IP", "IP_SET_HASH_NET", "IP_SET_HASH_IPPORT", "IP_SET_HASH_IPPORTNET",
            "IP_SET_BITMAP_PORT", "IP_SET_LIST_SET", "NETFILTER_XT_SET",
        ),
        "MACVLAN" to listOf("MACVLAN"),
        "IPVLAN" to listOf("IPVLAN"),
        "VXLAN" to listOf("VXLAN"),
        "CHECKPOINT_RESTORE" to listOf("CHECKPOINT_RESTORE"),
        "AUTOFS_FS" to listOf("AUTOFS_FS"),
    )
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
    /** CGROUP_PIDS is built in, but pids is not on cgroup2 (a cgroup v1 hierarchy took it). */
    const val UNAVAILABLE = "unavailable"

    /** A throwaway cgroup, so the check leaves nothing named dsh-go behind. */
    private val PROBE = "echo dsh-ns-uid=$(id -u); " +
        NAMESPACES.values.joinToString(" ") { "unshare -$it true >/dev/null 2>&1 && echo dsh-ns-$it=ok;" } +
        " d=/sys/fs/cgroup/dsh-check-$$; mkdir \$d 2>/dev/null && { test -e \$d/cgroup.kill && echo dsh-cg2=ok; rmdir \$d; }; " +
        "grep -qw pids /sys/fs/cgroup/cgroup.controllers && echo dsh-pids=ok; " +
        "zcat /proc/config.gz 2>/dev/null | grep -E '^(# )?CONFIG_(" + KERNEL_OPTIONS.values.flatten().joinToString("|") + ")[= ]'"

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

    /**
     * The kernel items over the probe's lines (`zcat /proc/config.gz` and `dsh-pids=ok`): [OFF] with
     * the options not built in (`=y`), [UNSUPPORTED] when the config cannot be read, [UNAVAILABLE] when
     * pids is built in but not on cgroup2.
     */
    fun kernelOptions(lines: List<String>, root: Boolean): List<Item> {
        val readable = lines.any { it.startsWith("CONFIG_") || it.startsWith("# CONFIG_") }
        return KERNEL_OPTIONS.map { (key, options) ->
            // Built in: a GKI device loads no modules of ours.
            val missing = options.filterNot { "CONFIG_$it=y" in lines }
            when {
                !root -> Item("kconfig:$key", false, NEEDS_ROOT)
                !readable -> Item("kconfig:$key", false, UNSUPPORTED)
                missing.isNotEmpty() -> Item("kconfig:$key", false, OFF, missing)
                key == "CGROUP_PIDS" && "dsh-pids=ok" !in lines -> Item("kconfig:$key", false, UNAVAILABLE)
                else -> Item("kconfig:$key", true)
            }
        }
    }

    private fun su(args: List<String>): RootShell.Output = RootShell.run(listOf("/system/bin/su") + args, timeoutMs = 10_000)
}
