package io.github.ccch1mneyyy.dsh_app.runtime

import java.io.File
import org.junit.Assert.*
import org.junit.Test

class SystemContainerTest {
    private val layout = SystemContainer.Layout(File("/data/user/0/app/files"), File("/data/app/x/lib/arm64"))
    private val prootRootfs = File("/data/user/0/app/files/runtime/rootfs")
    private val guest = listOf("/usr/bin/env", "-i", "HOME=/root", "DEEPSEEK_API_KEY=sk-x y", "/bin/sh", "-c",
        "echo \$\$ > /tmp/dsh-web.pid; exec /usr/local/bin/dsh web --host 127.0.0.1 --port 3080 --no-open")

    /** Inverse of DirectLaunch.quote: 'a'\''b' words separated by spaces. */
    private fun shellWords(command: String): List<String> {
        val words = ArrayList<String>()
        val word = StringBuilder()
        var quoted = false
        var at = 0
        while (at < command.length) {
            val c = command[at++]
            when {
                c == '\'' -> quoted = !quoted
                c == '\\' && !quoted -> word.append(command[at++])
                c == ' ' && !quoted -> { words += word.toString(); word.clear() }
                else -> word.append(c)
            }
        }
        words += word.toString()
        return words
    }

    @Test fun startBindsTheHomeAndDshLayerTheProotEtcReadOnlyAndStorage() {
        val args = SystemContainer.startArgs(layout, prootRootfs, listOf("/storage/emulated/0:/sdcard", "/storage"),
            10346, 10346, listOf(3003, 9997))
        assertEquals(listOf("/data/app/x/lib/arm64/libdshns.so", "start", "--name", "dsh", "--state", "/data/user/0/app/files/system/state",
            "--rootfs", "/data/user/0/app/files/system/rootfs", "--uid", "10346", "--gid", "10346", "--range", "2100000000:1048575",
            "--storage", "/data/user/0/app/files/system/storage.img", "--storage-size", "${32L shl 30}",
            "--groups", "3003,9997"), args.take(20))
        assertEquals("--net", args.last())
        val binds = args.drop(20).dropLast(1).chunked(2).onEach { assertEquals("--bind", it[0]) }.map { it[1] }
        val r = prootRootfs.path
        assertEquals(listOf("$r/root:/root", "$r/opt/dsh:/opt/dsh", "$r/opt/node:/opt/node", "$r/opt/python:/opt/python",
            "$r/opt/dsh-go:/opt/dsh-go", "$r/opt/dsh-app:/opt/dsh-app", "$r/usr/local:/usr/local", "$r/etc/dsh-app:/etc/dsh-app",
            "$r/var/lib/dsh-app:/var/lib/dsh-app", "$r/etc:/run/dsh-go/proot-etc:ro",
            "/storage/emulated/0:/sdcard", "/storage:/storage"), binds)
    }

    @Test fun dockerTakeoverChangesOnlyTheSignatureNotTheLaunchersArguments() {
        val args = SystemContainer.startArgs(layout, prootRootfs, emptyList(), 10346, 10346, emptyList())
        assertEquals(listOf("--net"), args.takeLast(1))
        assertFalse(args.any { it.contains("docker") })
        assertNotEquals(SystemContainer.signature("r15", args), SystemContainer.signature("r15", args, dockerTakeover = true))
        assertEquals(SystemContainer.signature("r15", args), SystemContainer.signature("r15", args, dockerTakeover = false))
        // A new daemon.json in an app update restarts the container, so Docker reads it.
        assertTrue(SystemContainer.signature("r15", args, dockerTakeover = true).contains("\"ip-forward-no-drop\": true"))
    }

    @Test fun execWithAForwardMapsTheAppLoopbackPort() {
        val proot = listOf("/lib/libproot.so", "--rootfs=/r", "--cwd=/root/workspace") + guest
        val command = SystemContainer.execCommand(layout, proot, 3080)
        assertEquals(listOf("/data/app/x/lib/arm64/libdshns.so", "exec", "--name", "dsh", "--state", "/data/user/0/app/files/system/state",
            "--cwd", "/root/workspace", "--forward", "3080", "--") + guest, shellWords(command[4]))
    }

    @Test fun startWithoutGroupsOrStorageLeavesThemOut() {
        val args = SystemContainer.startArgs(layout, prootRootfs, emptyList(), 10346, 10346, emptyList())
        assertFalse(args.contains("--groups"))
        assertFalse(args.any { it.startsWith("/storage") })
    }

    @Test fun execRunsTheGuestCommandUnchangedAtItsCwdThroughSuWithoutEscalation() {
        val proot = listOf("/lib/libproot.so", "--kill-on-exit", "--rootfs=/r", "--cwd=/root/workspace", "-b", "/dev",
            "-b", "/storage/emulated/0:/sdcard") + guest
        val command = SystemContainer.execCommand(layout, proot)
        assertEquals(listOf("/system/bin/su", "-W", "--ksu-no-new-privs", "-c"), command.take(4))
        assertEquals(5, command.size)
        assertEquals(listOf("/data/app/x/lib/arm64/libdshns.so", "exec", "--name", "dsh", "--state", "/data/user/0/app/files/system/state",
            "--cwd", "/root/workspace", "--") + guest, shellWords(command[4]))
    }

    @Test fun anAppUpdateKeepsTheContainerButNewBindsOrRuntimeRestartIt() {
        fun args(lib: String, storage: List<String> = emptyList()) =
            SystemContainer.startArgs(SystemContainer.Layout(File("/data/user/0/app/files"), File(lib)), prootRootfs, storage, 10346, 10346, emptyList())
        val before = SystemContainer.signature("r15", args("/data/app/~~a/lib/arm64"))
        assertEquals(before, SystemContainer.signature("r15", args("/data/app/~~b/lib/arm64")))
        assertNotEquals(before, SystemContainer.signature("r16", args("/data/app/~~a/lib/arm64")))
        assertNotEquals(before, SystemContainer.signature("r15", args("/data/app/~~a/lib/arm64", listOf("/storage"))))
    }

    @Test fun backupAndRestoreRunWithTheStorageAndOnlyTheUsersHome() {
        val args = SystemContainer.runArgs(layout, prootRootfs, 10346, 10346, listOf(3003), "echo hi")
        assertEquals(listOf("/data/app/x/lib/arm64/libdshns.so", "run"), args.take(2))
        assertTrue(args.containsAll(listOf("--storage", "/data/user/0/app/files/system/storage.img", "--groups", "3003")))
        assertEquals(listOf("--bind", "${prootRootfs.path}/root:/root", "--", "/usr/bin/bash", "-c", "echo hi"), args.takeLast(6))
        assertEquals(1, args.count { it == "--bind" })
    }

    @Test fun aRestoreKeepsAGrownCeilingAndRoomForTheBackup() {
        val gb = 1L shl 30
        assertEquals(32 * gb, SystemContainer.restoredStorageSize(null, 200L shl 20))
        assertEquals(48 * gb, SystemContainer.restoredStorageSize(48 * gb, 200L shl 20))
        // 10 GB compressed: room for 40, rounded up to the 16 GB step.
        assertEquals(48 * gb, SystemContainer.restoredStorageSize(32 * gb, 10 * gb))
        assertTrue(SystemContainer.runArgs(layout, prootRootfs, 1, 1, emptyList(), "x", 48 * gb).contains("${48 * gb}"))
    }

    @Test fun guestKeepsTheCommandFromEnvAndReadsCwdAndBinds() {
        val guest = SystemContainer.guest(listOf("/lib/libproot.so", "--cwd=/root/workspace", "-b", "/dev",
            "-b", "/storage/emulated/0:/sdcard", "--bind=/storage") + this.guest)
        assertEquals("/root/workspace", guest.cwd)
        assertEquals(listOf("/dev" to "/dev", "/storage/emulated/0" to "/sdcard", "/storage" to "/storage"), guest.binds)
        assertEquals(this.guest, guest.argv)
        assertThrows(IllegalArgumentException::class.java) { SystemContainer.guest(listOf("proot", "-b", "/dev")) }
        assertThrows(IllegalArgumentException::class.java) { SystemContainer.guest(listOf("proot", "-b", "/data:/") + this.guest) }
    }

    @Test fun readsGroupsAndFallsBackOnlyForTheRuntimeItFailedWith() {
        assertEquals(listOf(3003, 9997, 20346), SystemContainer.supplementaryGroups("Name:\tx\nGroups:\t3003 9997 20346 \nVmPeak:\t1"))
        assertEquals(emptyList<Int>(), SystemContainer.supplementaryGroups("Groups:\t\n"))
        assertTrue(SystemContainer.Preference(true, null, "r15").requested)
        assertFalse(SystemContainer.Preference(true, "r15", "r15").requested)
        assertTrue(SystemContainer.Preference(true, "r14", "r15").requested)
        assertFalse(SystemContainer.Preference(false, null, "r15").requested)
    }

    @Test fun execPidComesOnlyFromTheLaunchersLine() {
        assertEquals(4242, SystemContainer.execPid("[dshns] exec pid=4242"))
        assertNull(SystemContainer.execPid("[dshns] exec pid=0"))
        assertNull(SystemContainer.execPid("[dshns] container is not running"))
        assertNull(SystemContainer.execPid("dsh web: [dshns] exec pid=1"))
    }
}
