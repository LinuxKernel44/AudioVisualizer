package com.linuxkernel44.audiovisualizer

import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** Process-wide export state, written by [ExportService] and observed by the UI. */
object ExportController {
    var running by mutableStateOf(false)
    var phase by mutableStateOf("")
    var progress by mutableStateOf(0f)
    var renderFps by mutableStateOf(0f)
    var resultUri by mutableStateOf<Uri?>(null)
    var error by mutableStateOf<String?>(null)
    @Volatile var cancelRequested = false

    fun reset() {
        running = true
        phase = "Starting"
        progress = 0f
        renderFps = 0f
        resultUri = null
        error = null
        cancelRequested = false
    }
}
