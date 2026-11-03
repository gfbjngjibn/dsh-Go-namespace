package io.github.ccch1mneyyy.dsh_app.runtime

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.widget.Toast
import io.github.ccch1mneyyy.dsh_app.R
import io.github.ccch1mneyyy.dsh_app.i18n.AppStrings

/**
 * 「退出 DSH Go（含容器）」 from 引擎页 or the engine's notification: confirm, stop the engine,
 * let the system container's systemd shut down (cut short after a minute), then end the app.
 * The container is a daemon otherwise: closing or killing the app leaves it running.
 */
class QuitActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (quitting) return finish()
        AlertDialog.Builder(this, dialogTheme())
            .setTitle(AppStrings.get(this, R.string.quit_title))
            .setMessage(AppStrings.get(this, R.string.quit_message))
            .setNegativeButton(AppStrings.get(this, R.string.quit_cancel)) { _, _ -> finish() }
            .setPositiveButton(AppStrings.get(this, R.string.quit_confirm)) { _, _ -> quit() }
            .setOnCancelListener { finish() }
            .show()
    }

    private fun quit() {
        quitting = true
        AlertDialog.Builder(this, dialogTheme())
            .setMessage(AppStrings.get(this, R.string.quit_progress))
            .setCancelable(false)
            .show()
        val app = applicationContext
        RuntimeController.get(app).quit { down ->
            if (!down) Toast.makeText(app, AppStrings.get(app, R.string.quit_container_failed), Toast.LENGTH_LONG).show()
            (app.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).appTasks.forEach { runCatching { it.finishAndRemoveTask() } }
            // Long enough for the toast to be read.
            Handler(Looper.getMainLooper()).postDelayed({ Process.killProcess(Process.myPid()) }, if (down) 0L else 3000L)
        }
    }

    private fun dialogTheme(): Int =
        if (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK == Configuration.UI_MODE_NIGHT_YES) {
            android.R.style.Theme_DeviceDefault_Dialog_Alert
        } else {
            android.R.style.Theme_DeviceDefault_Light_Dialog_Alert
        }

    companion object {
        @Volatile private var quitting = false

        fun intent(context: Context): Intent =
            Intent(context, QuitActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
    }
}
