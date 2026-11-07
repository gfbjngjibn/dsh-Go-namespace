package io.github.ccch1mneyyy.dsh_app.runtime

import org.junit.Assert.*
import org.junit.Test

class SystemCheckTest {
    @Test fun trialPassesWhenSuRunsAsTheAppOrNotAtAll() {
        val ok = SystemCheck.trialResult(listOf("su-uid 10346"))
        assertTrue(ok.ok)
        assertNull(ok.detail)
        assertTrue(SystemCheck.trialResult(listOf("su-uid -")).ok)
    }

    @Test fun suThatRunsAsRootIsAnEscalation() {
        val escalated = SystemCheck.trialResult(listOf("su-uid 0"))
        assertFalse(escalated.ok)
        assertEquals(SystemCheck.ESCALATED, escalated.detail)
    }

    @Test fun aLauncherFailureReportsItsOwnLine() {
        val failed = SystemCheck.trialResult(listOf("[dshns] trial needs --uid and --gid"))
        assertFalse(failed.ok)
        assertEquals("[dshns] trial needs --uid and --gid", failed.detail)
        assertEquals(SystemCheck.TIMED_OUT, SystemCheck.trialResult(emptyList(), SystemCheck.TIMED_OUT).detail)
        assertEquals(SystemCheck.UNSUPPORTED, SystemCheck.trialResult(emptyList()).detail)
    }

    @Test fun kernelOptionsAreRequiredAndBuiltIn() {
        val all = SystemCheck.KERNEL_OPTIONS.values.flatten().map { "CONFIG_$it=y" } + "dsh-pids=ok"
        assertTrue(SystemCheck.kernelOptions(all, root = true).all { it.ok })

        val items = SystemCheck.kernelOptions(all.filterNot { it == "CONFIG_NFT_FIB_INET=y" || it == "CONFIG_NFT_CT=y" } +
            "# CONFIG_NFT_CT is not set" - "CONFIG_BRIDGE_NETFILTER=y" + "CONFIG_BRIDGE_NETFILTER=m", root = true)
            .associateBy { it.key }
        val nft = items.getValue("kconfig:NF_TABLES")
        assertFalse(nft.ok)
        assertEquals(SystemCheck.OFF, nft.detail)
        // Off or not in the config at all (a dependency is off): both missing.
        assertEquals(listOf("NFT_CT", "NFT_FIB_INET"), nft.missing)
        // A module is not enough: nothing loads it on a GKI device.
        assertEquals(listOf("BRIDGE_NETFILTER"), items.getValue("kconfig:BRIDGE_NETFILTER").missing)
        assertTrue(items.getValue("kconfig:IP_SET").ok)
    }

    @Test fun pidsMustBeOnCgroup2() {
        val config = SystemCheck.KERNEL_OPTIONS.values.flatten().map { "CONFIG_$it=y" }
        val pids = SystemCheck.kernelOptions(config, root = true).first { it.key == "kconfig:CGROUP_PIDS" }
        assertFalse(pids.ok)
        assertEquals(SystemCheck.UNAVAILABLE, pids.detail)
    }

    @Test fun anUnreadableConfigOrNoRootFails() {
        assertTrue(SystemCheck.kernelOptions(listOf("dsh-ns-uid=0"), root = true).all { !it.ok && it.detail == SystemCheck.UNSUPPORTED })
        assertTrue(SystemCheck.kernelOptions(emptyList(), root = false).all { !it.ok && it.detail == SystemCheck.NEEDS_ROOT })
    }
}
