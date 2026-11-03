package io.github.ccch1mneyyy.dsh_app.runtime

import android.content.Context
import android.content.res.AssetManager
import android.os.SystemClock
import android.system.Os
import io.github.ccch1mneyyy.dsh_app.R
import io.github.ccch1mneyyy.dsh_app.i18n.AppStrings
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.security.DigestInputStream
import java.security.MessageDigest
import org.json.JSONObject

/**
 * The system container (runtime/container/dshns.c): systemd as PID 1 in a user namespace
 * whose root is this app's uid, started as root through `su --ksu-no-new-privs` by the dshns
 * launcher (jniLib libdshns.so), sharing the host network. It is a daemon: the app being
 * killed or updated leaves it running; only [stop] (or a reboot) ends it.
 *
 * Its image (assets/runtime/system-<abi>.tar.xz) is unpacked once into files/system/rootfs and
 * never replaced by an app update. The PRoot rootfs's DSH layer and the user's /root are bound
 * in at their usual paths, so the engine is the runtime's own, and a runtime update reaches the
 * container when [start] restarts it (it does when the binds changed).
 */
object SystemContainer {
    /** Fallback reason: the system container launch already failed with this runtime. */
    const val FAILED = "systemFailed"
    /** dshns exec's stderr line naming the engine's host PID. */
    const val EXEC_PREFIX = "[dshns] exec pid="
    private const val NAME = "dsh"
    private const val MANIFEST = "${RuntimeManifest.ASSET_DIR}/system.json"
    /** The image's own record of what it is (rtbuild.py system-image). */
    private const val IMAGE_INFO = "var/lib/dsh-go/image.json"
    /** Host uids for the container's own users (uid 1 and up): far above any app's. */
    private const val RANGE = "2100000000:1048575"
    /** systemd's shutdown; then dshns kills what is left. */
    private const val STOP_TIMEOUT_S = 60
    /** PRoot rootfs paths bound into the container at the same path: the user's home and the DSH layer. */
    val SHARED = listOf(
        "root", "opt/dsh", "opt/node", "opt/python", "opt/dsh-go", "opt/dsh-app", "usr/local", "etc/dsh-app", "var/lib/dsh-app",
    )
    /** The PRoot rootfs's /etc, read-only: the image's resolv.conf links to the one the app writes there. */
    private const val PROOT_ETC = "/run/dsh-go/proot-etc"
    /**
     * Docker's and Podman's storage (dshns: an ext4 image, as overlayfs refuses /data): sparse,
     * so only what is in it takes space; this is its ceiling, which [grow] raises.
     */
    const val STORAGE_SIZE = 32L shl 30
    /** [grow]'s step. */
    const val STORAGE_STEP = 16L shl 30
    /** Where backups go and restores come from (needs 「访问手机文件」). */
    const val BACKUP_DIR = "/storage/emulated/0/Download/DSH-Go/backups"
    private const val BACKUP_PREFIX = "dsh-system-"
    private const val BACKUP_SUFFIX = ".tar.gz"
    /**
     * Run in the container's own view of users (so a backup survives the app's uid changing):
     * the image, the storage and /root. Not /tmp, the API filesystems mounted for the run
     * (tar would call them changed), nor what the runtime brings back by itself.
     */
    private const val BACKUP_SCRIPT = "set -o pipefail; cd / && tar --create --file=- --numeric-owner --xattrs " +
        "--xattrs-include='*' --acls --sparse --one-file-system --exclude=./proc --exclude=./sys --exclude=./dev " +
        "--exclude=./run --exclude=./tmp/* --exclude=./var/tmp/* " +
        "--exclude=./var/cache/apt/archives/*.deb --exclude=./var/lib/dsh-go/storage/lost+found " +
        ". ./var/lib/dsh-go/storage | gzip -1"
    private const val RESTORE_SCRIPT = "set -o pipefail; cd / && gzip -dc | tar --extract --file=- --numeric-owner " +
        "--xattrs --xattrs-include='*' --acls --preserve-permissions --same-owner"

    data class Image(val version: String, val asset: String, val sha256: String?, val bytes: Long, val uncompressedBytes: Long)

    /** [saved] is the user's choice; [failedRuntime] the runtime version it last failed to launch with. */
    data class Preference(val saved: Boolean, val failedRuntime: String?, val runtime: String?) {
        val failed: Boolean get() = failedRuntime != null && failedRuntime == runtime
        val requested: Boolean get() = saved && !failed
    }

    /** What a proot argv asks of the guest: its `--cwd`, its binds (host to guest) and the command from `/usr/bin/env` on. */
    data class Guest(val cwd: String, val binds: List<Pair<String, String>>, val argv: List<String>)

    fun guest(prootArgv: List<String>): Guest {
        val envAt = prootArgv.indexOf("/usr/bin/env")
        require(envAt > 0) { "system container: launch command has no guest part" }
        var cwd = "/root"
        val binds = ArrayList<Pair<String, String>>()
        var at = 0
        while (at < envAt) {
            val arg = prootArgv[at++]
            val bind = when {
                arg.startsWith("--cwd=") -> { cwd = arg.substringAfter('='); null }
                arg == "-b" || arg == "--bind" -> prootArgv.getOrNull(at++) ?: error("system container: missing bind value")
                arg.startsWith("--bind=") -> arg.substringAfter('=')
                else -> null
            } ?: continue
            val host = bind.substringBefore(':')
            val path = bind.substringAfter(':', host)
            require(host.startsWith('/') && path.startsWith('/') && path != "/") { "system container: unsupported bind $bind" }
            binds += host to path
        }
        return Guest(cwd, binds, prootArgv.drop(envAt))
    }

    /** This process's supplementary groups (inet among them: sockets need it on Android). */
    fun supplementaryGroups(status: String = File("/proc/self/status").readText()): List<Int> =
        status.lineSequence().firstOrNull { it.startsWith("Groups:") }
            ?.substringAfter(':')?.trim()?.split(Regex("\\s+"))?.mapNotNull { it.toIntOrNull() }.orEmpty()

    /** The image this APK carries, or null (none for this ABI). */
    fun bundled(context: Context): Image? = try {
        val json = JSONObject(context.assets.open(MANIFEST).bufferedReader().use { it.readText() })
        Image(json.getString("imageVersion"), json.getString("image"), json.optString("imageSha256").ifEmpty { null },
            json.optLong("imageBytes", -1), json.optLong("uncompressedBytes", -1))
    } catch (_: FileNotFoundException) {
        null
    }

    /** Where the container lives: files/system, and the launcher among the native libraries. */
    class Layout(filesDir: File, nativeLibraryDir: File) {
        val dir = File(filesDir, "system")
        val rootfs = File(dir, "rootfs")
        val staging = File(dir, "rootfs.partial")
        /** dshns's state: lock, init.pid, console and monitor logs. */
        val state = File(dir, "state")
        /** What the running container was started with; a start with different binds restarts it. */
        val started = File(state, "started")
        val storage = File(dir, "storage.img")
        val launcher = File(nativeLibraryDir, "libdshns.so")
    }

    fun layout(context: Context) = Layout(context.filesDir, File(context.applicationInfo.nativeLibraryDir))

    /** Version of the unpacked image, or null when none is. */
    fun installedVersion(context: Context): String? = try {
        JSONObject(File(layout(context).rootfs, IMAGE_INFO).readText()).optString("imageVersion").ifEmpty { null }
    } catch (_: Exception) {
        null
    }

    /**
     * Running since this boot. The launcher's processes are out of our sight (the root
     * manager's SELinux domain), so this reads its state file: the launcher removes it when
     * the container's init is gone, and one from before the last boot is stale.
     */
    fun running(context: Context): Boolean {
        val pid = File(layout(context).state, "init.pid")
        return pid.isFile && pid.lastModified() > System.currentTimeMillis() - SystemClock.elapsedRealtime()
    }

    /** Unpacks the bundled image, verified, unless one is already there (it is never replaced). */
    fun install(context: Context, image: Image, onProgress: (Long, Long) -> Unit, onLog: (String) -> Unit) {
        if (installedVersion(context) != null) return
        val root = layout(context).rootfs
        check(!root.exists()) { "system container: ${root.path} holds no image record; delete the container first" }
        val staging = layout(context).staging
        staging.deleteRecursivelySafely()
        val need = (image.uncompressedBytes.takeIf { it > 0 } ?: (800L shl 20)) * 115 / 100
        val free = context.filesDir.usableSpace
        check(free >= need) { AppStrings.get(context, R.string.service_storage_insufficient, need shr 20, free shr 20) }
        val asset = "${RuntimeManifest.ASSET_DIR}/${image.asset}"
        val started = System.currentTimeMillis()
        val digest = MessageDigest.getInstance("SHA-256")
        DigestInputStream(context.assets.open(asset, AssetManager.ACCESS_STREAMING), digest).use { input ->
            val buffer = ByteArray(1 shl 16)
            while (input.read(buffer) >= 0) {}
        }
        val actual = digest.digest().joinToString("") { "%02x".format(it) }
        if (image.sha256 != null && !image.sha256.equals(actual, ignoreCase = true)) {
            throw IllegalStateException(AppStrings.get(context, R.string.service_package_checksum_failed))
        }
        staging.mkdirs()
        var reported = 0L
        val progress: (Long) -> Unit = { read ->
            if (read - reported >= 512 * 1024) {
                reported = read
                onProgress(read, image.bytes)
            }
        }
        val extractor = TarExtractor(staging, progress)
        if (image.asset.endsWith(".xz")) {
            extractor.extractDecoded(ParallelXZInputStream.open({ AssetSeekableInputStream(context.assets.openFd(asset)) }, progress))
        } else {
            context.assets.open(asset, AssetManager.ACCESS_STREAMING).use { extractor.extract(it, image.asset) }
        }
        // The archive has no entry for / itself, and our umask made it 0700: the container's
        // users other than root (messagebus, syslog...) could not even look up a path.
        Os.chmod(staging.path, "755".toInt(8))
        if (!staging.renameTo(root)) throw IOException("system container: cannot move ${staging.path} into place")
        onProgress(image.bytes, image.bytes)
        onLog("[dsh-app] system container image ${image.version} unpacked in ${(System.currentTimeMillis() - started) / 1000}s")
    }

    /** Host PID of the engine from one `dshns exec` stderr line, or null. */
    fun execPid(line: String): Int? =
        if (line.startsWith(EXEC_PREFIX)) line.removePrefix(EXEC_PREFIX).trim().toIntOrNull()?.takeIf { it > 0 } else null

    /** dshns's arguments for this container. */
    private fun dshns(layout: Layout, command: String, vararg args: String) =
        listOf(layout.launcher.path, command, "--name", NAME, "--state", layout.state.path) + args

    /** The launcher through su, as a root that can never escalate again. */
    private fun su(argv: List<String>) =
        listOf("/system/bin/su", "-W", "--ksu-no-new-privs", "-c", argv.joinToString(" ", transform = DirectLaunch::quote))

    /**
     * `dshns start`: [storage] holds PhoneStorage's binds (host[:guest]) when the app may reach
     * the phone's files. `--net` gives the container its own network namespace (and marks
     * containers started before it had one, so that they restart).
     */
    fun startArgs(layout: Layout, prootRootfs: File, storage: List<String>, uid: Int, gid: Int, groups: List<Int>): List<String> {
        val args = ArrayList(containerArgs(layout, "start", uid, gid, groups))
        for (path in SHARED) args += listOf("--bind", "${File(prootRootfs, path).path}:/$path")
        args += listOf("--bind", "${File(prootRootfs, "etc").path}:$PROOT_ETC:ro")
        for (bind in storage) args += listOf("--bind", "${bind.substringBefore(':')}:${bind.substringAfter(':', bind)}")
        args += "--net"
        return args
    }

    /** What `dshns start` and `dshns run` share: the rootfs, its users and the storage ([storageSize]: if it is created). */
    private fun containerArgs(layout: Layout, command: String, uid: Int, gid: Int, groups: List<Int>, storageSize: Long = STORAGE_SIZE): List<String> {
        val args = ArrayList(dshns(layout, command, "--rootfs", layout.rootfs.path, "--uid", "$uid", "--gid", "$gid", "--range", RANGE,
            "--storage", layout.storage.path, "--storage-size", "$storageSize"))
        if (groups.isNotEmpty()) args += listOf("--groups", groups.joinToString(","))
        return args
    }

    /** `dshns run` of a bash script with the stopped container's root, storage and the user's /root. */
    fun runArgs(layout: Layout, prootRootfs: File, uid: Int, gid: Int, groups: List<Int>, script: String, storageSize: Long = STORAGE_SIZE): List<String> =
        containerArgs(layout, "run", uid, gid, groups, storageSize) + listOf("--bind", "${File(prootRootfs, "root").path}:/root", "--",
            "/usr/bin/bash", "-c", script)

    /**
     * `dshns exec` of a proot argv's guest command, run as the container's root at its cwd.
     * [forwardPort] relays the host's 127.0.0.1:<port> to the same port inside the container;
     * the engine keeps binding 127.0.0.1 (dsh web refuses a wildcard address), so the app keeps
     * its loopback origin (a secure context).
     */
    fun execCommand(layout: Layout, prootArgv: List<String>, forwardPort: Int? = null): List<String> {
        val guest = guest(prootArgv)
        val forward = if (forwardPort != null) listOf("--forward", "$forwardPort") else emptyList()
        return su(dshns(layout, "exec", "--cwd", guest.cwd) + forward + listOf("--") + guest.argv)
    }

    /**
     * Starts the container unless it runs (blocking, seconds). One started with other binds,
     * or from another runtime, is shut down and started again: its mounts would still be the
     * old ones. [runtime] is the runtime version the binds come from; [dockerTakeover] whether
     * the app owns Docker's daemon.json, which Docker reads only when the container boots.
     */
    fun start(context: Context, args: List<String>, runtime: String, dockerTakeover: Boolean = false, log: (String) -> Unit) {
        val layout = layout(context)
        val signature = signature(runtime, args, dockerTakeover)
        if (running(context)) {
            val was = runCatching { layout.started.readText() }.getOrNull()
            if (was == signature) return
            log("[dsh-app] system container runs with an older runtime or other binds; restarting it")
            check(stop(context, log)) { "system container did not stop" }
        }
        layout.state.mkdirs()
        val out = RootShell.run(su(args), timeoutMs = 60_000)
        if (out.lines.none { it.startsWith("running ") }) {
            throw IOException("system container did not start: " +
                (out.lines.lastOrNull { it.startsWith("[dshns] ") } ?: out.failure ?: out.lines.lastOrNull() ?: "no answer"))
        }
        layout.started.writeText(signature)
        log("[dsh-app] system container ${out.lines.last { it.startsWith("running ") }}")
    }

    /** The daemon.json the app writes when it takes Docker over. */
    private val DOCKER_CONFIG = """
        {
          "bridge": "docker0",
          "iptables": true,
          "ip6tables": false,
          "ip-forward-no-drop": true
        }
    """.trimIndent() + "\n"

    /**
     * rootfs/etc/docker/daemon.json, or null when etc/docker does not resolve to that very
     * directory: the container owns what is in its rootfs, and a symlink there must not make
     * the app write (or delete) one of its own files.
     */
    private fun dockerConfig(context: Context): java.nio.file.Path? {
        val rootfs = layout(context).rootfs.canonicalFile
        val dir = File(rootfs, "etc/docker")
        if (!dir.isDirectory && !dir.mkdirs()) return null
        if (dir.canonicalPath != dir.path) return null
        val file = File(dir, "daemon.json").toPath()
        return file.takeUnless { java.nio.file.Files.isSymbolicLink(it) }
    }

    /**
     * The app takes Docker's daemon configuration over: a bridge Docker can create inside the
     * container's netns, iptables on the legacy backend (the kernel has no nf_tables), no
     * IPv6 table, and FORWARD's policy left at ACCEPT (Docker's DROP would also drop the
     * phone's connections to ports Podman publishes). Called before the container starts; the
     * daemon reads it at boot.
     */
    fun takeoverDocker(context: Context, log: (String) -> Unit): Boolean = try {
        val file = dockerConfig(context) ?: error("etc/docker is not a plain directory of the rootfs")
        if (java.nio.file.Files.isRegularFile(file) && String(java.nio.file.Files.readAllBytes(file)) == DOCKER_CONFIG) {
            false
        } else {
            java.nio.file.Files.newByteChannel(file, setOf(java.nio.file.StandardOpenOption.WRITE,
                java.nio.file.StandardOpenOption.CREATE, java.nio.file.StandardOpenOption.TRUNCATE_EXISTING,
                java.nio.file.LinkOption.NOFOLLOW_LINKS)).use { it.write(java.nio.ByteBuffer.wrap(DOCKER_CONFIG.toByteArray())) }
            log("[dsh-app] Docker configuration taken over by the app")
            true
        }
    } catch (error: Exception) {
        log("[dsh-app] cannot write Docker configuration: ${error.message}")
        false
    }

    /** Takeover turned off: removes the app's daemon.json, unless someone has edited it since. */
    fun releaseDocker(context: Context, log: (String) -> Unit) {
        runCatching {
            val file = dockerConfig(context) ?: return
            if (java.nio.file.Files.isRegularFile(file) && String(java.nio.file.Files.readAllBytes(file)) == DOCKER_CONFIG) {
                java.nio.file.Files.delete(file)
                log("[dsh-app] Docker configuration handed back (the app's daemon.json removed)")
            }
        }
    }

    /**
     * 「局域网访问容器」: dshns lets anything routed to the container's subnet in while
     * state/lan exists, and checks every two seconds, so a running container follows at once.
     */
    fun setLanAccess(context: Context, enabled: Boolean) {
        val state = layout(context).state
        val flag = File(state, "lan")
        if (enabled) {
            state.mkdirs()
            flag.createNewFile()
        } else {
            flag.delete()
        }
    }

    /**
     * What a running container must have been started with to be kept: the runtime and the
     * start arguments, but not the launcher's path (an app update moves the native libraries,
     * and the container is no different for it), and the daemon.json the app owns, if any.
     */
    fun signature(runtime: String, startArgs: List<String>, dockerTakeover: Boolean = false): String =
        (listOf(runtime) + startArgs.drop(1) + if (dockerTakeover) listOf("docker-takeover", DOCKER_CONFIG) else emptyList()).joinToString("\n")

    /** Asks systemd to shut down, which dshns cuts short after [STOP_TIMEOUT_S]; blocking. */
    fun stop(context: Context, log: (String) -> Unit): Boolean {
        val layout = layout(context)
        val out = RootShell.run(su(dshns(layout, "stop", "--timeout", "$STOP_TIMEOUT_S")), timeoutMs = (STOP_TIMEOUT_S + 20) * 1000L)
        val stopped = out.lines.contains("stopped")
        if (stopped) layout.started.delete()
        log("[dsh-app] system container ${if (stopped) "stopped" else "stop failed: ${out.lines.lastOrNull() ?: out.failure}"}")
        return stopped
    }

    /** The storage image's ceiling and what it takes on /data, in bytes; null before the first start. */
    fun storageUse(context: Context): Pair<Long, Long>? {
        val file = layout(context).storage
        return runCatching { Os.stat(file.path).let { it.st_size to it.st_blocks * 512 } }.getOrNull()
    }

    /** Raises the storage's ceiling by [STORAGE_STEP]: at once while the container runs, else at its next start. */
    fun grow(context: Context, log: (String) -> Unit): Long {
        val layout = layout(context)
        val size = (storageUse(context)?.first ?: STORAGE_SIZE) + STORAGE_STEP
        val out = RootShell.run(su(dshns(layout, "grow", "--storage", layout.storage.path, "--storage-size", "$size")), timeoutMs = 60_000)
        val now = out.lines.firstOrNull { it.startsWith("size ") }?.removePrefix("size ")?.toLongOrNull()
            ?: throw IOException("storage did not grow: " + (out.lines.lastOrNull() ?: out.failure ?: "no answer"))
        log("[dsh-app] system container storage ceiling now ${now shr 30} GB")
        return now
    }

    data class Backup(val name: String, val bytes: Long, val modified: Long)

    /** Backups in [BACKUP_DIR], newest first. */
    fun backups(): List<Backup> = File(BACKUP_DIR).listFiles { f -> f.isFile && f.name.startsWith(BACKUP_PREFIX) && f.name.endsWith(BACKUP_SUFFIX) }
        .orEmpty().map { Backup(it.name, it.length(), it.lastModified()) }.sortedByDescending { it.modified }

    /**
     * Writes everything of the stopped container (its system, the storage, /root) to a new
     * file in [BACKUP_DIR]; returns its name. Blocking, minutes for a big storage.
     */
    fun backup(context: Context, prootRootfs: File, uid: Int, gid: Int, groups: List<Int>, log: (String) -> Unit): String {
        check(!running(context)) { "stop the system container first" }
        check(installedVersion(context) != null) { "no system container to back up" }
        val dir = File(BACKUP_DIR)
        check(dir.isDirectory || dir.mkdirs()) { "cannot create $BACKUP_DIR" }
        val name = BACKUP_PREFIX + java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.ROOT).format(java.util.Date()) + BACKUP_SUFFIX
        val partial = File(dir, "$name.partial")
        val started = System.currentTimeMillis()
        val code = try {
            CountingOutputStream(partial.outputStream().buffered(1 shl 20)) { log("[dsh-app] system container backup: ${it shr 20} MB") }.use { out ->
                RootShell.stream(su(runArgs(layout(context), prootRootfs, uid, gid, groups, BACKUP_SCRIPT)), null, out) { log(it) }
            }
        } catch (e: Exception) {
            partial.delete()
            throw e
        }
        if (code != 0) {
            partial.delete()
            throw IOException("backup failed (exit $code)")
        }
        check(partial.renameTo(File(dir, name))) { "cannot finish $name" }
        log("[dsh-app] system container backed up to $name (${File(dir, name).length() shr 20} MB) in ${(System.currentTimeMillis() - started) / 1000}s")
        return name
    }

    /**
     * Replaces the container with backup [name]: deletes it, unpacks the bundled image for its
     * tools, then extracts the backup over it (the image's own files it no longer had stay).
     */
    fun restore(context: Context, image: Image, name: String, prootRootfs: File, uid: Int, gid: Int, groups: List<Int>, log: (String) -> Unit) {
        val file = File(BACKUP_DIR, name)
        require(name.startsWith(BACKUP_PREFIX) && name.endsWith(BACKUP_SUFFIX) && '/' !in name && file.isFile) { "no backup $name" }
        val storageSize = restoredStorageSize(storageUse(context)?.first, file.length())
        remove(context, log)
        install(context, image, { _, _ -> }, log)
        val started = System.currentTimeMillis()
        val code = file.inputStream().buffered(1 shl 20).use { input ->
            RootShell.stream(su(runArgs(layout(context), prootRootfs, uid, gid, groups, RESTORE_SCRIPT, storageSize)), input, null) { log(it) }
        }
        if (code != 0) throw IOException("restore failed (exit $code); delete the system container and try again")
        log("[dsh-app] system container restored from $name in ${(System.currentTimeMillis() - started) / 1000}s")
    }

    /**
     * The new storage's ceiling for a restore: the one being replaced, or room for four times
     * the (compressed) backup, whichever is more. A backup does not record its ceiling, and
     * sparse room costs nothing.
     */
    fun restoredStorageSize(current: Long?, backupBytes: Long): Long {
        val room = (backupBytes * 4 + STORAGE_STEP - 1) / STORAGE_STEP * STORAGE_STEP
        return maxOf(STORAGE_SIZE, current ?: 0, room)
    }

    /** Counts what passes, reporting every 256 MB. */
    private class CountingOutputStream(out: java.io.OutputStream, private val report: (Long) -> Unit) : java.io.FilterOutputStream(out) {
        private var count = 0L
        override fun write(b: ByteArray, off: Int, len: Int) {
            out.write(b, off, len)
            val before = count
            count += len
            if (count shr 28 != before shr 28) report(count)
        }
        override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)
    }

    /**
     * Deletes the container's files. Its users own some of them, so this takes real root
     * (plain su); the container must be stopped first.
     */
    fun remove(context: Context, log: (String) -> Unit) {
        if (running(context)) check(stop(context, log)) { "system container did not stop" }
        val layout = layout(context)
        val targets = listOf(layout.rootfs, layout.staging, layout.state, layout.storage).filter { it.exists() }
        if (targets.isEmpty()) return
        val out = RootShell.run(listOf("/system/bin/su", "-c", "rm -rf -- " + targets.joinToString(" ") { DirectLaunch.quote(it.path) }),
            timeoutMs = 180_000)
        check(targets.none { it.exists() }) { "system container: delete failed: ${out.lines.lastOrNull() ?: out.failure}" }
        log("[dsh-app] system container deleted")
    }
}
