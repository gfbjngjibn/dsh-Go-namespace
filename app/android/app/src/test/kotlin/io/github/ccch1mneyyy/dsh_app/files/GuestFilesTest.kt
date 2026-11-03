package io.github.ccch1mneyyy.dsh_app.files

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Test

class GuestFilesTest {
    private val proot = File("/data/user/0/app/files/runtime/rootfs")
    private val system = File("/data/user/0/app/files/system/rootfs")

    private fun host(path: String, system: File?) = GuestFiles.hostOf(proot, system, path.split('/').filter { it.isNotEmpty() }).path

    @Test fun underProotEverythingIsInItsRootfs() {
        assertEquals("$proot/srv/report.pdf", host("/srv/report.pdf", null))
        assertEquals("$proot/root/workspace/a.md", host("/root/workspace/a.md", null))
    }

    @Test fun inTheSystemContainerOnlyTheSharedPathsAreProots() {
        assertEquals("$proot/root/workspace/a.md", host("/root/workspace/a.md", system))
        assertEquals("$proot/opt/dsh/x", host("/opt/dsh/x", system))
        assertEquals("$proot/usr/local/bin/dsh", host("/usr/local/bin/dsh", system))
        assertEquals("$system/srv/report.pdf", host("/srv/report.pdf", system))
        assertEquals("$system/tmp/out.png", host("/tmp/out.png", system))
        assertEquals("$system/rootfile", host("/rootfile", system))
        assertEquals("$system/opt/other/x", host("/opt/other/x", system))
    }
}
