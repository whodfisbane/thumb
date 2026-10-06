package dev.thumb.app.core

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageInstaller
import android.os.Build
import java.io.File
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

/** Installs an APK through Android's PackageInstaller (the user confirms). */
object Installer {
    private const val ACTION = "dev.thumb.app.INSTALL_RESULT"

    sealed class Outcome {
        data object Success : Outcome()
        data class Failure(val status: Int, val message: String) : Outcome()
    }

    suspend fun install(context: Context, apk: File): Outcome = install(context, listOf(apk))

    /** Installs a base APK plus its splits in one session. */
    suspend fun install(context: Context, apks: List<File>): Outcome {
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        if (Build.VERSION.SDK_INT >= 34) params.setRequestUpdateOwnership(false)
        val sessionId = installer.createSession(params)
        installer.openSession(sessionId).use { session ->
            for ((i, apk) in apks.withIndex()) {
                session.openWrite(if (i == 0) "base.apk" else "split_$i.apk", 0, apk.length()).use { out ->
                    apk.inputStream().use { it.copyTo(out) }
                    session.fsync(out)
                }
            }
            return suspendCancellableCoroutine { cont ->
                val receiver = object : BroadcastReceiver() {
                    override fun onReceive(ctx: Context, intent: Intent) {
                        if (intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1) != sessionId) return
                        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
                        when (status) {
                            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                                @Suppress("DEPRECATION")
                                val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                                confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                                if (confirm != null) ctx.startActivity(confirm)
                            }
                            else -> {
                                ctx.unregisterReceiver(this)
                                val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "status $status"
                                if (cont.isActive) cont.resume(
                                    if (status == PackageInstaller.STATUS_SUCCESS) Outcome.Success else Outcome.Failure(status, message)
                                )
                            }
                        }
                    }
                }
                context.registerReceiver(receiver, IntentFilter(ACTION), Context.RECEIVER_NOT_EXPORTED)
                val intent = Intent(ACTION).setPackage(context.packageName)
                val pending = PendingIntent.getBroadcast(context, sessionId, intent, PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
                session.commit(pending.intentSender)
            }
        }
    }
}
