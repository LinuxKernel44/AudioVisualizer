package com.linuxkernel44.audiovisualizer

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.provider.MediaStore
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Runs one native export in the foreground so it survives the app being backgrounded. The audio
 * source and the settings arrive in the start intent; the MP4 is written into MediaStore
 * (Movies/AudioVisualizer) and published when complete.
 */
class ExportService : Service() {
    private var worker: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_CANCEL) {
            ExportController.cancelRequested = true
            return START_NOT_STICKY
        }
        if (worker?.isAlive == true || intent == null) return START_NOT_STICKY
        val audio = intent.getParcelableExtraCompat<Uri>(EXTRA_AUDIO) ?: return START_NOT_STICKY
        val cfg = ExportConfig(
            shortSide = intent.getIntExtra(EXTRA_SHORT_SIDE, 1080),
            aspect = Aspect.values()[intent.getIntExtra(EXTRA_ASPECT, 0)],
            fps = intent.getIntExtra(EXTRA_FPS, 30),
            quality = Quality.values()[intent.getIntExtra(EXTRA_QUALITY, 1)],
        )
        val controls = Controls(
            intent.getFloatExtra("sensitivity", 1f), intent.getFloatExtra("gainBass", 0f), intent.getFloatExtra("gainMid", 0f),
            intent.getFloatExtra("gainTreble", 0f), intent.getFloatExtra("smoothing", 0.3f), intent.getIntExtra("palette", -1),
            intent.getFloatExtra("beatFlash", 1f), intent.getFloatExtra("bloom", 1f),
        )
        val scene = intent.getIntExtra(EXTRA_SCENE, -1)
        val title = intent.getStringExtra(EXTRA_TITLE) ?: "AudioVisualizer"

        createChannel()
        val type = if (Build.VERSION.SDK_INT >= 35) ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROCESSING
        else ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
        ServiceCompat.startForeground(this, NOTIF_ID, buildNotification("Starting export…", 0, true), type)
        ExportController.reset()
        wakeLock = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AudioVisualizer:export").apply { acquire(6 * 60 * 60 * 1000L) }

        worker = Thread({ runExport(audio, cfg, controls, scene, title) }, "export").also { it.start() }
        return START_NOT_STICKY
    }

    private fun runExport(audio: Uri, cfg: ExportConfig, controls: Controls, scene: Int, title: String) {
        val resolver = contentResolver
        var outUri: Uri? = null
        var handle = 0L
        try {
            val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
            val safe = title.substringBeforeLast('.').replace(Regex("[^A-Za-z0-9_-]+"), "_").take(40)
            val values = ContentValues().apply {
                put(MediaStore.Video.Media.DISPLAY_NAME, "AudioVisualizer_${safe}_$stamp.mp4")
                put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/AudioVisualizer")
                put(MediaStore.Video.Media.IS_PENDING, 1)
            }
            outUri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
                ?: throw IllegalStateException("Could not create the output file")
            val cacheDir = java.io.File(cacheDir, "analysis").apply { mkdirs() }.absolutePath
            val audioPfd = resolver.openFileDescriptor(audio, "r") ?: throw IllegalStateException("Cannot open the audio file")
            val outPfd = resolver.openFileDescriptor(outUri, "rw") ?: throw IllegalStateException("Cannot open the output file")
            audioPfd.use { a ->
                outPfd.use { o ->
                    handle = NativeExport.start(
                        a.fd, o.fd, cfg.width, cfg.height, cfg.fps, cfg.videoBitrate, 192_000, scene, 1f,
                        controls.sensitivity, controls.gainBass, controls.gainMid, controls.gainTreble,
                        controls.smoothing, controls.palette, controls.beatFlash, controls.bloom, cacheDir,
                    )
                    // The native job duplicated both descriptors; poll until it finishes.
                    while (true) {
                        if (ExportController.cancelRequested) NativeExport.cancel(handle)
                        val st = NativeExport.state(handle)
                        ExportController.progress = NativeExport.progress(handle)
                        ExportController.phase = NativeExport.phase(handle)
                        ExportController.renderFps = NativeExport.renderFps(handle)
                        updateNotification(ExportController.phase, (ExportController.progress * 100).toInt())
                        if (st != 0) {
                            when (st) {
                                1 -> Unit
                                3 -> throw CancellationException()
                                else -> throw IllegalStateException(NativeExport.error(handle).ifBlank { "Export failed" })
                            }
                            break
                        }
                        Thread.sleep(300)
                    }
                }
            }
            resolver.update(outUri, ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }, null, null)
            ExportController.resultUri = outUri
            ExportController.progress = 1f
            notifyDone("Export complete", "Saved to Movies/AudioVisualizer", outUri)
        } catch (e: CancellationException) {
            outUri?.let { runCatching { resolver.delete(it, null, null) } }
            notifyDone("Export cancelled", "", null)
        } catch (e: Exception) {
            outUri?.let { runCatching { resolver.delete(it, null, null) } }
            ExportController.error = e.message ?: "Export failed"
            notifyDone("Export failed", e.message ?: "", null)
        } finally {
            if (handle != 0L) NativeExport.release(handle)
            ExportController.running = false
            wakeLock?.let { if (it.isHeld) it.release() }
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_DETACH)
            stopSelf()
        }
    }

    private class CancellationException : Exception()

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Video export", NotificationManager.IMPORTANCE_LOW))
    }

    private fun buildNotification(text: String, percent: Int, indeterminate: Boolean = false): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val cancel = PendingIntent.getService(this, 1, Intent(this, ExportService::class.java).setAction(ACTION_CANCEL), PendingIntent.FLAG_IMMUTABLE)
        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle("Exporting video")
            .setContentText(text)
            .setProgress(100, percent, indeterminate)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(open)
            .addAction(0, "Cancel", cancel)
            .build()
    }

    private fun updateNotification(text: String, percent: Int) {
        getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification(text, percent))
    }

    private fun notifyDone(title: String, text: String, uri: Uri?) {
        val nm = getSystemService(NotificationManager::class.java)
        val b = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_download_done)
            .setContentTitle(title)
            .setContentText(text)
            .setAutoCancel(true)
        if (uri != null) {
            val view = Intent(Intent.ACTION_VIEW).setDataAndType(uri, "video/mp4").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            b.setContentIntent(PendingIntent.getActivity(this, 2, view, PendingIntent.FLAG_IMMUTABLE))
        }
        nm.notify(NOTIF_ID + 1, b.build())
    }

    companion object {
        const val ACTION_CANCEL = "com.linuxkernel44.audiovisualizer.CANCEL_EXPORT"
        const val EXTRA_AUDIO = "audio"
        const val EXTRA_SHORT_SIDE = "shortSide"
        const val EXTRA_ASPECT = "aspect"
        const val EXTRA_FPS = "fps"
        const val EXTRA_QUALITY = "quality"
        const val EXTRA_SCENE = "scene"
        const val EXTRA_TITLE = "title"
        private const val CHANNEL = "export"
        private const val NOTIF_ID = 42

        fun start(ctx: Context, audio: Uri, title: String, cfg: ExportConfig, controls: Controls, scene: Int) {
            val i = Intent(ctx, ExportService::class.java)
                .putExtra(EXTRA_AUDIO, audio).putExtra(EXTRA_TITLE, title)
                .putExtra(EXTRA_SHORT_SIDE, cfg.shortSide).putExtra(EXTRA_ASPECT, cfg.aspect.ordinal)
                .putExtra(EXTRA_FPS, cfg.fps).putExtra(EXTRA_QUALITY, cfg.quality.ordinal).putExtra(EXTRA_SCENE, scene)
                .putExtra("sensitivity", controls.sensitivity).putExtra("gainBass", controls.gainBass)
                .putExtra("gainMid", controls.gainMid).putExtra("gainTreble", controls.gainTreble)
                .putExtra("smoothing", controls.smoothing).putExtra("palette", controls.palette)
                .putExtra("beatFlash", controls.beatFlash).putExtra("bloom", controls.bloom)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            ctx.startForegroundService(i)
        }
    }
}

@Suppress("DEPRECATION")
private inline fun <reified T : android.os.Parcelable> Intent.getParcelableExtraCompat(name: String): T? =
    if (Build.VERSION.SDK_INT >= 33) getParcelableExtra(name, T::class.java) else getParcelableExtra(name)
