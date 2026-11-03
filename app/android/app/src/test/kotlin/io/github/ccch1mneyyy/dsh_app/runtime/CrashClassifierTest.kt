package io.github.ccch1mneyyy.dsh_app.runtime

import org.junit.Assert.*
import org.junit.Test

class CrashClassifierTest {
    private val fatal = listOf("dsh: startup failed: required row missing")

    @Test fun classifiesAllFiveFailureKinds() {
        fun classify(ready: Long?, now: Long, tail: List<String>) =
            CrashClassifier.classify(137, ready, now, 123L, tail, false).kind
        assertEquals(FailureKind.KILLED, classify(1L, 180_000L, listOf("normal output")))
        assertEquals(FailureKind.STARTUP_FAILED, classify(null, 1L, fatal))
        assertEquals(FailureKind.CRASHED_EARLY, classify(1L, 120_001L, fatal))
        assertEquals(FailureKind.CRASHED_LATE, classify(1L, 120_002L, fatal))
        assertEquals(FailureKind.NO_SPACE, classify(null, 1L, fatal + "ENOSPC"))
    }

    @Test fun summariesNeverContainLaunchTokens() {
        val failure = CrashClassifier.classify(1, null, 1L, 1L,
            listOf("dsh: startup failed: http://localhost/?token=private-value"), false)
        assertFalse(failure.summary!!.contains("private-value"))
        assertTrue(failure.summary!!.contains("token="))
    }

    @Test fun aSystemContainerLauncherFailureIsALaunchFailureOnlyForThatLaunch() {
        val tail = listOf("[dsh-app] launch method: system", "[dshns] container is not running")
        val failed = CrashClassifier.classify(125, null, 1L, 1L, tail, false, systemEngine = true)
        assertTrue(SafePolicy.directLaunchFailed(failed, true))
        assertNotEquals(FailureKind.LAUNCH_FAILED, CrashClassifier.classify(125, null, 1L, 1L, tail, false).kind)
        // The launcher's own PID note is not a failure.
        val note = CrashClassifier.classify(1, null, 1L, 1L, listOf("[dshns] exec pid=42"), false, systemEngine = true)
        assertNotEquals(FailureKind.LAUNCH_FAILED, note.kind)
    }
}
