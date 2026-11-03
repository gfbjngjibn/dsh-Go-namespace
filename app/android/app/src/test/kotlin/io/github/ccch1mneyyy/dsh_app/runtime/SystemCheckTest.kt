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

    @Test fun kernelOptionsAreWarningsReadFromTheConfig() {
        val items = SystemCheck.kernelOptions(listOf("dsh-ns-uid=0", "# CONFIG_CGROUP_PIDS is not set",
            "CONFIG_NF_TABLES=y", "CONFIG_BRIDGE_NETFILTER=m", "CONFIG_AUTOFS_FS=n"), root = true).associateBy { it.key }
        assertTrue(items.values.all { it.warn })
        assertEquals(SystemCheck.OFF, items.getValue("kconfig:CGROUP_PIDS").detail)
        assertTrue(items.getValue("kconfig:NF_TABLES").ok)
        assertTrue(items.getValue("kconfig:BRIDGE_NETFILTER").ok)
        assertEquals(SystemCheck.OFF, items.getValue("kconfig:AUTOFS_FS").detail)
        // Not in the config at all (or /proc/config.gz unreadable): unknown, not off.
        assertEquals(SystemCheck.UNSUPPORTED, items.getValue("kconfig:CHECKPOINT_RESTORE").detail)
        assertEquals(SystemCheck.NEEDS_ROOT, SystemCheck.kernelOptions(emptyList(), root = false).first().detail)
    }
}
