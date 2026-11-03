package io.github.ccch1mneyyy.dsh_app.files

import android.content.Context
import io.github.ccch1mneyyy.dsh_app.runtime.PhoneStorage
import io.github.ccch1mneyyy.dsh_app.runtime.RuntimeController
import io.github.ccch1mneyyy.dsh_app.runtime.SystemContainer
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption

/**
 * Where a path of the local Linux environment lives on Android.
 *
 * proot maps the guest root onto the rootfs (`filesDir/runtime/rootfs`, see
 * RuntimeInstaller) and, with phone storage on, binds the shared storage at
 * `/sdcard` and `/storage` (PhoneStorage.binds). Symlinks inside the rootfs
 * hold *guest* targets (`/usr/lib/…`, and `--link2symlink` turns hard links
 * into links into `/root/.l2s`), so each one is followed in the guest world,
 * not by Android.
 *
 * With the engine in the system container, its own rootfs (`filesDir/system/rootfs`)
 * is the guest root, and only the paths it shares with PRoot's (SystemContainer.SHARED:
 * `/root`, the DSH layer) live in the PRoot rootfs.
 */
object GuestFiles {
    private const val MAX_LINKS = 16

    fun rootfs(context: Context): File = File(context.filesDir, "runtime/rootfs")

    /** Host file for an absolute guest [path]; null when it is not a readable regular file. */
    fun resolve(context: Context, path: String): File? {
        if (!path.startsWith("/")) return null
        val rootfs = rootfs(context)
        val system = if (RuntimeController.get(context).engineInSystemContainer()) SystemContainer.layout(context).rootfs else null
        val pending = ArrayDeque(segments(path))
        val resolved = ArrayList<String>()
        var links = 0
        while (pending.isNotEmpty()) {
            val segment = pending.removeFirst()
            when (segment) {
                "", "." -> continue
                ".." -> { if (resolved.isNotEmpty()) resolved.removeAt(resolved.size - 1); continue }
            }
            resolved += segment
            val host = hostOf(rootfs, system, resolved)
            val nio = host.toPath()
            if (!Files.isSymbolicLink(nio)) continue
            if (++links > MAX_LINKS) return null
            val target = try { Files.readSymbolicLink(nio).toString() } catch (_: Exception) { return null }
            resolved.removeAt(resolved.size - 1)
            if (target.startsWith("/")) {
                // A link that already names a host path (the shared storage) needs no mapping.
                if (target.startsWith(PhoneStorage.hostRoot())) {
                    val rest = pending.joinToString("/")
                    val file = if (rest.isEmpty()) File(target) else File(target, rest)
                    return file.takeIf { it.isFile && it.canRead() }
                }
                resolved.clear()
            }
            segments(target).reversed().forEach { pending.addFirst(it) }
        }
        val file = hostOf(rootfs, system, resolved)
        return file.takeIf { Files.isRegularFile(it.toPath(), LinkOption.NOFOLLOW_LINKS) && it.canRead() }
    }

    /** Host path of resolved guest [segments] (no symlinks left to follow in them); [system]: the system container's rootfs. */
    internal fun hostOf(rootfs: File, system: File?, segments: List<String>): File {
        val guest = "/" + segments.joinToString("/")
        val phone = PhoneStorage.GUEST_PATH
        return when {
            guest == phone || guest.startsWith("$phone/") -> File(PhoneStorage.hostRoot() + guest.removePrefix(phone))
            guest == "/storage" || guest.startsWith("/storage/") -> File(guest)
            system != null && SystemContainer.SHARED.none { guest == "/$it" || guest.startsWith("/$it/") } ->
                File(system, guest.removePrefix("/"))
            else -> File(rootfs, guest.removePrefix("/"))
        }
    }

    private fun segments(path: String): List<String> = path.split('/').filter { it.isNotEmpty() }
}
