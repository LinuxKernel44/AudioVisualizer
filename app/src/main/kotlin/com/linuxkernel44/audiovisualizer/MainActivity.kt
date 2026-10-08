package com.linuxkernel44.audiovisualizer

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
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {
    private val vm: PlayerViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContent {
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

@Composable
private fun PlayerScreen(vm: PlayerViewModel) {
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.open(uri)
    }
    LaunchedEffect(Unit) {
        while (true) {
            vm.refresh()
            delay(250)
        }
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(h: SurfaceHolder) {}
                        override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {
                            vm.engine.setSurface(h.surface)
                        }
                        override fun surfaceDestroyed(h: SurfaceHolder) {
                            vm.engine.setSurface(null)
                        }
                    })
                }
            },
        )
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
                    var dragging by remember { mutableStateOf<Float?>(null) }
                    val shown = dragging ?: vm.positionMs.toFloat()
                    Slider(
                        value = shown.coerceIn(0f, vm.durationMs.toFloat().coerceAtLeast(1f)),
                        onValueChange = { dragging = it },
                        onValueChangeFinished = {
                            dragging?.let { vm.seekTo(it.toLong()) }
                            dragging = null
                        },
                        valueRange = 0f..vm.durationMs.toFloat().coerceAtLeast(1f),
                    )
                    Text("${formatTime(shown.toLong())} / ${formatTime(vm.durationMs)}", color = Color.White)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.padding(top = 8.dp)) {
                    Button(onClick = {
                        picker.launch(arrayOf("audio/*"))
                    }) { Text("Import") }
                    Button(onClick = { vm.togglePlay() }, enabled = vm.trackName != null) {
                        Text(if (vm.playing) "Pause" else "Play")
                    }
                }
            }
        }
    }
}
