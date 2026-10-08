package com.linuxkernel44.audiovisualizer

import android.app.Application
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import java.util.concurrent.Executors

class PlayerViewModel(app: Application) : AndroidViewModel(app) {
    val engine = NativeEngine()
    private val worker = Executors.newSingleThreadExecutor()

    var trackName by mutableStateOf<String?>(null)
        private set
    var loading by mutableStateOf(false)
        private set
    var error by mutableStateOf<String?>(null)
        private set
    var playing by mutableStateOf(false)
        private set
    var positionMs by mutableStateOf(0L)
        private set
    var durationMs by mutableStateOf(0L)
        private set

    fun open(uri: Uri) {
        loading = true
        error = null
        worker.execute {
            val resolver = getApplication<Application>().contentResolver
            val name = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
            val pfd = try {
                resolver.openFileDescriptor(uri, "r")
            } catch (e: Exception) {
                null
            }
            val err = if (pfd == null) {
                "Cannot open the selected file"
            } else {
                pfd.use { engine.load(it.fd) }  // native side duplicates the fd
            }
            loading = false
            if (err == null) {
                trackName = name ?: "Audio"
                durationMs = engine.durationMs()
                positionMs = 0
                engine.play()
                playing = true
            } else {
                error = err
            }
        }
    }

    fun seekTo(ms: Long) {
        if (trackName == null) return
        positionMs = ms
        worker.execute {  // seeking blocks briefly on the decoder; keep it off the UI thread
            engine.seekTo(ms)
            refresh()
        }
    }

    fun togglePlay() {
        if (trackName == null) return
        if (engine.isPlaying()) engine.pause() else engine.play()
        refresh()
    }

    /** Polled from the UI at a low rate; cheap JNI getters only. */
    fun refresh() {
        playing = engine.isPlaying()
        positionMs = engine.positionMs()
    }

    override fun onCleared() {
        worker.shutdownNow()
        engine.release()
    }
}
