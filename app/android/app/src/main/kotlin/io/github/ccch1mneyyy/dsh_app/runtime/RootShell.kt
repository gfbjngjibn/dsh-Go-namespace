package io.github.ccch1mneyyy.dsh_app.runtime

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/**
 * Runs one root command (`su ...`) to completion and collects its merged output: the
 * requirement checks' probes and the system container's dshns calls. Blocking: call off
 * the main thread.
 */
internal object RootShell {
    /** [failure]: null once the command exited in time, else [NOT_GRANTED] or [TIMED_OUT]. */
    class Output(val lines: List<String>, val failure: String?, val exitCode: Int? = null)

    const val NOT_GRANTED = "notGranted"
    const val TIMED_OUT = "timeout"

    /** [keepStdin]: launchers that stop their guest when stdin closes get it closed only at the end. */
    fun run(command: List<String>, keepStdin: Boolean = false, timeoutMs: Long): Output {
        val p = try {
            ProcessBuilder(command).redirectErrorStream(true).start()
        } catch (_: IOException) {
            // KernelSU hides su from apps it has not granted.
            return Output(emptyList(), NOT_GRANTED)
        }
        if (!keepStdin) runCatching { p.outputStream.close() }
        val lines = ArrayList<String>()
        val reader = thread(name = "dsh-root-shell", isDaemon = true) {
            runCatching { p.inputStream.bufferedReader().forEachLine { synchronized(lines) { lines += it.trim() } } }
        }
        val done = p.waitFor(timeoutMs, TimeUnit.MILLISECONDS)
        runCatching { p.outputStream.close() }
        if (!done) {
            p.destroy()
            p.waitFor(8, TimeUnit.SECONDS)
        }
        reader.join(2000)
        return Output(synchronized(lines) { lines.toList() }, if (done) null else TIMED_OUT, if (done) p.exitValue() else null)
    }

    /**
     * Runs one root command over binary streams (the system container's backup and restore):
     * [input] feeds its stdin, its stdout goes to [output], its stderr lines to [onError].
     * Returns its exit code. No timeout: it takes as long as the data does.
     */
    fun stream(command: List<String>, input: InputStream?, output: OutputStream?, onError: (String) -> Unit): Int {
        val p = try {
            ProcessBuilder(command).start()
        } catch (_: IOException) {
            throw IOException(NOT_GRANTED)
        }
        val errors = thread(name = "dsh-root-stream-err", isDaemon = true) {
            runCatching { p.errorStream.bufferedReader().forEachLine { onError(it.trim()) } }
        }
        val feeder = if (input == null) {
            runCatching { p.outputStream.close() }
            null
        } else thread(name = "dsh-root-stream-in", isDaemon = true) {
            // A command that stops reading early closes the pipe: its exit code tells why.
            runCatching { p.outputStream.use { input.copyTo(it, 1 shl 16) } }
        }
        p.inputStream.use { stdout ->
            if (output != null) stdout.copyTo(output, 1 shl 16)
            else ByteArray(1 shl 16).let { buffer -> while (stdout.read(buffer) >= 0) {} }
        }
        val code = p.waitFor()
        feeder?.join()
        errors.join(2000)
        return code
    }
}
