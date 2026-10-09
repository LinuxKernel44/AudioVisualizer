package com.linuxkernel44.audiovisualizer

import android.app.Application
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

val SCENE_NAMES = listOf("Bars", "Radial", "Tunnel", "Fractal", "Particles")

class PlayerViewModel(app: Application) : AndroidViewModel(app) {
    val engine = NativeEngine()
    /** Serialises every engine transport call (load, play, pause, seek), which may block. */
    private val worker = Executors.newSingleThreadExecutor()
    /** Draws branding layers, so a slow bitmap never delays playback. */
    private val brandingWorker = Executors.newSingleThreadExecutor()

    private fun onWorker(block: () -> Unit) {
        try {
            worker.execute(block)
        } catch (_: RejectedExecutionException) {  // the view model is being cleared
        }
    }

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
    var analysisRunning by mutableStateOf(false)
        private set
    var analysisProgress by mutableStateOf(0f)
        private set
    var analysisError by mutableStateOf<String?>(null)
        private set
    var summary by mutableStateOf<AnalysisSummary?>(null)
        private set
    /** -1 = automatic (the director follows the song structure), otherwise a scene index. */
    var scene by mutableStateOf(-1)
        private set

    fun selectScene(index: Int) {
        scene = index
        engine.setScene(index)
    }

    fun cycleScene(delta: Int) {
        val n = SCENE_NAMES.size
        val base = if (scene < 0) 0 else scene
        selectScene(((base + delta) % n + n) % n)
    }

    private val prefs = app.getSharedPreferences("controls", android.content.Context.MODE_PRIVATE)
    var controls by mutableStateOf(Controls.load(prefs))
        private set
    private val brandPrefs = app.getSharedPreferences("branding", android.content.Context.MODE_PRIVATE)
    var branding by mutableStateOf(Branding.load(brandPrefs))
        private set
    var showBranding by mutableStateOf(false)
    private var surfaceW = 0
    private var surfaceH = 0
    private var titleAutoFilled = true
    private val brandingGen = AtomicInteger()
    private var uploadedLayers: Any? = null  // what the renderer currently shows; brandingWorker only

    fun updateBranding(b: Branding) {
        // Manual edits stop automatic title/artist filling for the current track.
        if (b.title != branding.title || b.artist != branding.artist) titleAutoFilled = false
        branding = b
        b.save(brandPrefs)
        applyBranding()
    }

    fun onSurfaceSize(w: Int, h: Int) {
        if (w == surfaceW && h == surfaceH) return
        surfaceW = w
        surfaceH = h
        applyBranding()
    }

    /**
     * Rebuilds the branding layers at about half the surface size and hands them to the renderer.
     * Requests are coalesced (typing a title or dragging a slider sends many), and a change that
     * only affects opacity does not redraw the bitmaps.
     */
    private fun applyBranding() {
        val b = branding
        val w = surfaceW / 2 * 2
        val h = surfaceH / 2 * 2
        if (w < 16 || h < 16) return
        engine.setBrandingOpacity(b.overlayOpacity, if (b.hasBackground) b.backgroundOpacity else 0f)
        val gen = brandingGen.incrementAndGet()
        try {
            brandingWorker.execute {
                if (gen != brandingGen.get()) return@execute  // superseded by a newer request
                val sw = (w / 2).coerceAtLeast(16)
                val sh = (h / 2).coerceAtLeast(16)
                val key = Triple(b.copy(overlayOpacity = 0f, backgroundOpacity = 0f), sw, sh)
                if (key == uploadedLayers) return@execute
                val ctx = getApplication<Application>()
                engine.setOverlay(BrandingRenderer.overlay(ctx, b, sw, sh))
                engine.setBackground(BrandingRenderer.background(ctx, b, sw, sh))
                uploadedLayers = key
            }
        } catch (_: RejectedExecutionException) {
        }
    }

    private fun fillTitleFromMetadata(uri: Uri) {
        try {
            val r = android.media.MediaMetadataRetriever()
            r.setDataSource(getApplication<Application>(), uri)
            val t = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_TITLE)
            val a = r.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_ARTIST)
            r.release()
            if (titleAutoFilled || branding.title.isBlank()) {
                val nb = branding.copy(title = t ?: (trackName ?: "").substringBeforeLast('.'), artist = a ?: "")
                branding = nb
                nb.save(brandPrefs)
                titleAutoFilled = true
                applyBranding()
            }
        } catch (_: Exception) {
        }
    }

    // ---- settings, presets, updates -------------------------------------------------------------
    private val settingsPrefs = app.getSharedPreferences("settings", android.content.Context.MODE_PRIVATE)
    var showSettings by mutableStateOf(false)
    var batterySaver by mutableStateOf(settingsPrefs.getBoolean("batterySaver", false))
        private set
    var autoResolution by mutableStateOf(settingsPrefs.getBoolean("autoResolution", true))
        private set
    private val presetStore = PresetStore(app)
    var presets by mutableStateOf(presetStore.all())
        private set
    private val tokenStore = TokenStore(app)
    var hasToken by mutableStateOf(tokenStore.has())
        private set
    var updateStatus by mutableStateOf("")
        private set
    var updating by mutableStateOf(false)
        private set
    var cacheBytes by mutableStateOf(0L)
        private set

    fun setBatterySaverEnabled(on: Boolean) {
        batterySaver = on
        settingsPrefs.edit().putBoolean("batterySaver", on).apply()
        engine.setPowerSaver(on)
    }

    fun setAutoResolutionEnabled(on: Boolean) {
        autoResolution = on
        settingsPrefs.edit().putBoolean("autoResolution", on).apply()
        engine.setAutoScale(on)
    }

    fun openSettings() {
        showSettings = true
        refreshCache()
    }

    private fun cacheDir() = java.io.File(getApplication<Application>().cacheDir, "analysis")

    fun refreshCache() {
        onWorker { cacheBytes = cacheDir().listFiles()?.sumOf { it.length() } ?: 0L }
    }

    fun clearCache() {
        onWorker {
            cacheDir().listFiles()?.forEach { it.delete() }
            cacheBytes = 0L
        }
    }

    fun applyPreset(p: Preset) {
        selectScene(p.scene)
        updateControls(p.controls)
    }

    fun savePreset(name: String) {
        if (name.isBlank()) return
        presetStore.save(Preset(name.trim().take(40), scene, controls))
        presets = presetStore.all()
    }

    fun deletePreset(name: String) {
        presetStore.delete(name)
        presets = presetStore.all()
    }

    fun exportPresetsJson(): String = presetStore.exportUserJson()

    fun importPresets(ctx: android.content.Context, uri: Uri) {
        onWorker {
            updateStatus = try {
                val text = ctx.contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText().take(512 * 1024) } ?: throw IllegalStateException("Cannot read the file")
                "Imported ${presetStore.import(text)} preset(s)."
            } catch (e: Exception) {
                "Import failed: ${e.message}"
            }
            presets = presetStore.all()
        }
    }

    fun saveToken(t: String) {
        tokenStore.save(t)
        hasToken = tokenStore.has()
    }

    fun clearToken() {
        tokenStore.clear()
        hasToken = false
    }

    fun checkPresetUpdates() {
        if (updating) return
        updating = true
        updateStatus = "Checking…"
        onWorker {
            updateStatus = when (val r = PresetUpdater.update(presetStore, tokenStore.load())) {
                is PresetUpdater.Result.Updated -> "Updated: ${r.count} preset(s)."
                is PresetUpdater.Result.Failed -> r.message
            }
            presets = presetStore.all()
            updating = false
        }
    }

    private val recentStore = RecentTracks(app.getSharedPreferences("recent", android.content.Context.MODE_PRIVATE))
    var recents by mutableStateOf(recentStore.list())
        private set

    fun forgetRecent(uri: String) {
        recents = recentStore.remove(uri)
    }

    var showControls by mutableStateOf(false)
    var showPerf by mutableStateOf(false)
    var perfText by mutableStateOf("")
    var thermalStatus by mutableStateOf(0)
        private set

    fun onThermalStatus(status: Int) {
        thermalStatus = status
        engine.setThermalLevel(status)
    }

    fun pollPerf() {
        if (!showPerf) return
        val p = engine.perf()
        val fps = if (p[0] > 0f) 1000f / p[0] else 0f
        perfText = "%.0f fps (target %.0f)\nGPU %.1f ms\nscale %.2f\nthermal %d\naudio latency %.0f ms".format(fps, p[3], p[1], p[2], thermalStatus, p[4])
    }

    fun updateControls(c: Controls) {
        controls = c
        engine.setControls(c)
        c.save(prefs)
    }

    init {
        engine.setControls(controls)
        engine.setPowerSaver(batterySaver)
        engine.setAutoScale(autoResolution)
        onWorker { engine.setBeatModel(BeatModel.path(app)) }  // first launch copies the model: not on the UI thread
        engine.setBrandingOpacity(branding.overlayOpacity, if (branding.hasBackground) branding.backgroundOpacity else 0f)
        val dir = java.io.File(app.cacheDir, "analysis").apply { mkdirs() }
        engine.setCacheDir(dir.absolutePath)
    }

    var currentUri: Uri? = null
        private set
    var showExport by mutableStateOf(false)

    fun startExport(ctx: android.content.Context, cfg: ExportConfig) {
        val uri = currentUri ?: return
        showExport = false
        pausePlayback()
        ExportService.start(ctx, uri, trackName ?: "track", cfg, controls, scene)
    }

    fun dismissExportResult() {
        ExportController.resultUri = null
        ExportController.error = null
    }

    fun open(uri: Uri) {
        currentUri = uri
        loading = true
        error = null
        summary = null
        analysisError = null
        analysisRunning = false
        onWorker {
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
            if (err == null) {
                trackName = name ?: "Audio"
                recents = recentStore.add(RecentTrack(uri.toString(), trackName ?: "Audio"))
                durationMs = engine.durationMs()
                positionMs = 0
                startPlaybackNow()
                loading = false
                fillTitleFromMetadata(uri)
            } else {
                // The previous track was unloaded: nothing is playable any more.
                trackName = null
                currentUri = null
                durationMs = 0
                positionMs = 0
                playing = false
                error = err
                loading = false
            }
        }
    }

    fun seekTo(ms: Long) {
        if (trackName == null) return
        positionMs = ms
        onWorker {  // seeking blocks briefly on the decoder; keep it off the UI thread
            engine.seekTo(ms)
            refresh()
        }
    }

    private val guard = PlaybackGuard(app) { pausePlayback() }

    /** Pauses playback (focus loss, headphones unplugged, app in the background). */
    fun pausePlayback() {
        if (trackName == null) return
        onWorker {
            engine.pause()
            guard.release()
            refresh()
        }
    }

    /** Worker thread only. */
    private fun startPlaybackNow() {
        if (guard.acquire()) engine.play()
        refresh()
    }

    fun togglePlay() {
        if (trackName == null) return
        if (playing) pausePlayback() else onWorker { startPlaybackNow() }
    }

    /** Polled from the UI at a low rate; cheap, non-blocking JNI getters only. */
    fun refresh() {
        val wasPlaying = playing
        playing = engine.isPlaying()
        positionMs = engine.positionMs()
        if (wasPlaying && !playing && engine.isFinished()) guard.release()  // reached the end
        // While a file is being opened the engine still reports the previous track's analysis.
        if (trackName == null || loading) return
        when (engine.analysisState()) {
            1 -> {
                analysisRunning = true
                analysisProgress = engine.analysisProgress()
            }
            2 -> {
                analysisRunning = false
                if (summary == null) summary = AnalysisSummary.parse(engine.summaryJson())
            }
            3 -> {
                analysisRunning = false
                analysisError = engine.analysisError()
            }
            else -> analysisRunning = false
        }
    }

    override fun onCleared() {
        guard.release()
        brandingWorker.shutdownNow()
        try {
            brandingWorker.awaitTermination(2, TimeUnit.SECONDS)
        } catch (_: InterruptedException) {
        }
        // Released after any transport call still queued or running (they hold the native engine).
        onWorker { engine.release() }
        worker.shutdown()
    }
}
