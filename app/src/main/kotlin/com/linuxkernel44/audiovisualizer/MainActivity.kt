package com.linuxkernel44.audiovisualizer

import android.content.Intent
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {
    private val vm: PlayerViewModel by viewModels()
    private var thermalListener: android.os.PowerManager.OnThermalStatusChangedListener? = null

    override fun onStop() {
        super.onStop()
        // A visualizer has nothing to show in the background; exports keep running in their service.
        if (!ExportController.running) vm.pausePlayback()
    }

    /**
     * The fastest refresh rate the display offers at its current resolution, capped at 120 Hz (the
     * preview target). The current mode's rate is not enough: high-refresh phones report 60 Hz
     * until an app asks for more.
     */
    private fun previewFps(): Float {
        val d = display ?: return 60f
        val mode = d.mode
        val best = d.supportedModes
            .filter { it.physicalWidth == mode.physicalWidth && it.physicalHeight == mode.physicalHeight }
            .maxOfOrNull { it.refreshRate } ?: d.refreshRate
        return best.coerceIn(30f, 120f)
    }

    /**
     * Asks for the display mode matching the preview rate. The surface also requests its rate, but
     * some vendor policies only honour a window-level mode request for apps they do not know.
     */
    fun requestPreviewMode(enabled: Boolean) {
        if (!enabled) {  // battery saver: let the system pick (the renderer asks for 30 Hz)
            window.attributes = window.attributes.also { it.preferredDisplayModeId = 0 }
            return
        }
        val d = display ?: return
        val target = previewFps()
        val cur = d.mode
        val mode = d.supportedModes.firstOrNull {
            it.physicalWidth == cur.physicalWidth && it.physicalHeight == cur.physicalHeight &&
                kotlin.math.abs(it.refreshRate - target) < 1f
        } ?: return
        window.attributes = window.attributes.also { it.preferredDisplayModeId = mode.modeId }
    }

    override fun onDestroy() {
        // The listener references this activity; a recreated activity registers its own.
        thermalListener?.let { (getSystemService(android.content.Context.POWER_SERVICE) as android.os.PowerManager).removeThermalStatusListener(it) }
        thermalListener = null
        super.onDestroy()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    /** "Open with" / "Share to" audio intents. */
    private fun handleIntent(intent: Intent?) {
        val uri = when (intent?.action) {
            Intent.ACTION_VIEW -> intent.data
            Intent.ACTION_SEND -> @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_STREAM)
            else -> null
        } ?: return
        intent?.action = null  // do not reload on configuration change
        vm.open(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        handleIntent(intent)
        vm.engine.setTargetFps(previewFps())
        if (android.os.Build.VERSION.SDK_INT >= 29) {
            val pm = getSystemService(android.content.Context.POWER_SERVICE) as android.os.PowerManager
            val l = android.os.PowerManager.OnThermalStatusChangedListener { status -> vm.onThermalStatus(status) }
            pm.addThermalStatusListener(mainExecutor, l)
            thermalListener = l
            vm.onThermalStatus(pm.currentThermalStatus)
        }
        setContent {
            LaunchedEffect(vm.batterySaver) { requestPreviewMode(!vm.batterySaver) }
            MaterialTheme(colorScheme = darkColorScheme(background = Color.Black, surface = Color.Black)) {
                PlayerScreen(vm)
            }
        }
    }
}

private fun formatTime(ms: Long): String {
    val s = ms / 1000
    return "%d:%02d".format(s / 60, s % 60)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PlayerScreen(vm: PlayerViewModel) {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            try {  // keep access across restarts; some providers do not support it
                ctx.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (_: SecurityException) {
            }
            vm.open(uri)
        }
    }
    LaunchedEffect(Unit) {
        var tick = 0
        while (true) {
            vm.refresh()
            if (tick++ % 2 == 0) vm.pollPerf()
            delay(250)
        }
    }

    val notifPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    if (vm.showExport) {
        ExportDialog(onDismiss = { vm.showExport = false }) { cfg ->
            if (android.os.Build.VERSION.SDK_INT >= 33) notifPermission.launch(android.Manifest.permission.POST_NOTIFICATIONS)
            vm.startExport(ctx, cfg)
        }
    }
    ExportStatus(onDismissResult = { vm.dismissExportResult() })
    if (vm.showSettings) SettingsDialog(vm, onDismiss = { vm.showSettings = false })
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        var swipe by remember { mutableStateOf(0f) }
        AndroidView(
            modifier = Modifier.fillMaxSize().pointerInput(Unit) {
                detectHorizontalDragGestures(
                    onDragStart = { swipe = 0f },
                    onHorizontalDrag = { _, dx -> swipe += dx },
                    onDragEnd = {
                        if (swipe < -120f) vm.cycleScene(1) else if (swipe > 120f) vm.cycleScene(-1)
                        swipe = 0f
                    },
                )
            },
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(h: SurfaceHolder) {}
                        override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {
                            vm.onSurfaceSize(w, hh)
                            vm.engine.setSurface(h.surface)
                        }
                        override fun surfaceDestroyed(h: SurfaceHolder) {
                            vm.engine.setSurface(null)
                        }
                    })
                }
            },
        )
        Column(Modifier.fillMaxWidth().statusBarsPadding().padding(top = 36.dp)) {
        Row(
            Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            FilterChip(selected = vm.scene < 0, onClick = { vm.selectScene(-1) }, label = { Text("Auto") })
            SCENE_NAMES.forEachIndexed { i, n ->
                FilterChip(selected = vm.scene == i, onClick = { vm.selectScene(i) }, label = { Text(n) })
            }
            FilterChip(selected = vm.showControls, onClick = { vm.showControls = !vm.showControls; vm.showBranding = false }, label = { Text("Tune") })
            FilterChip(selected = vm.showSettings, onClick = { vm.openSettings() }, label = { Text("Settings") })
            FilterChip(selected = vm.showBranding, onClick = { vm.showBranding = !vm.showBranding; vm.showControls = false }, label = { Text("Brand") })
        }
        if (vm.showBranding) BrandingPanel(vm.branding, { vm.updateBranding(it) }, Modifier.padding(top = 8.dp))
        if (vm.showControls) ControlsPanel(vm.controls, { vm.updateControls(it) }, vm.showPerf, { vm.showPerf = it }, Modifier.padding(top = 8.dp))
        }
        if (vm.showPerf) {
            Text(
                vm.perfText,
                color = Color(0xFF8CFF98),
                fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
                fontSize = androidx.compose.ui.unit.TextUnit(11f, androidx.compose.ui.unit.TextUnitType.Sp),
                modifier = Modifier.align(Alignment.TopEnd).statusBarsPadding().padding(top = 110.dp, end = 12.dp),
            )
        }
        Column(
            Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(16.dp),
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(
                text = vm.error ?: vm.trackName ?: "Import an audio file to start",
                color = if (vm.error != null) Color(0xFFFF6B6B) else Color.White,
            )
            Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
                if (vm.loading) CircularProgressIndicator()
                if (vm.trackName != null) {
                    val sm = vm.summary
                    if (sm != null) {
                        Text(
                            "%.0f BPM  ·  %s  ·  %.1f LUFS  ·  %d/4%s".format(sm.bpm, sm.keyText, sm.lufs, sm.beatsPerBar, if (sm.beatSource == "ml") "  ·  ML" else ""),
                            color = Color.White,
                            modifier = Modifier.padding(bottom = 6.dp),
                        )
                        TimelineStrip(
                            summary = sm,
                            positionSec = vm.positionMs / 1000f,
                            onSeek = { vm.seekTo((it * 1000).toLong()) },
                        )
                    } else {
                        if (vm.analysisRunning) {
                            Text("Analyzing… ${(vm.analysisProgress * 100).toInt()}%", color = Color.White)
                            LinearProgressIndicator(progress = { vm.analysisProgress }, modifier = Modifier.fillMaxWidth())
                        }
                        vm.analysisError?.let { Text("Analysis failed: $it", color = Color(0xFFFF6B6B)) }
                        var dragging by remember { mutableStateOf<Float?>(null) }
                        val shownMs = dragging ?: vm.positionMs.toFloat()
                        Slider(
                            value = shownMs.coerceIn(0f, vm.durationMs.toFloat().coerceAtLeast(1f)),
                            onValueChange = { dragging = it },
                            onValueChangeFinished = {
                                dragging?.let { vm.seekTo(it.toLong()) }
                                dragging = null
                            },
                            valueRange = 0f..vm.durationMs.toFloat().coerceAtLeast(1f),
                        )
                    }
                    Text("${formatTime(vm.positionMs)} / ${formatTime(vm.durationMs)}", color = Color.White)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.padding(top = 8.dp)) {
                    Button(onClick = {
                        picker.launch(arrayOf("audio/*"))
                    }) { Text("Import") }
                    Button(onClick = { vm.togglePlay() }, enabled = vm.trackName != null) {
                        Text(if (vm.playing) "Pause" else "Play")
                    }
                    Button(onClick = { vm.showExport = true }, enabled = vm.trackName != null && vm.summary != null && !ExportController.running) {
                        Text("Export")
                    }
                }
            }
        }
    }
}
