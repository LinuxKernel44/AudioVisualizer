package com.linuxkernel44.audiovisualizer

import android.content.Intent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp

private val RESOLUTIONS = listOf(720 to "720p", 1080 to "1080p", 1440 to "1440p", 2160 to "4K")
private val FPS_OPTIONS = listOf(24, 30, 60, 120)

/** Choose resolution / ratio / frame rate / quality, then start the export. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ExportDialog(onDismiss: () -> Unit, onStart: (ExportConfig) -> Unit) {
    var cfg by remember { mutableStateOf(ExportConfig()) }
    val warning = remember(cfg) { cfg.capabilityWarning() }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Export video") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Resolution")
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    RESOLUTIONS.forEach { (s, n) -> FilterChip(selected = cfg.shortSide == s, onClick = { cfg = cfg.copy(shortSide = s) }, label = { Text(n) }) }
                }
                Text("Format")
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Aspect.values().forEach { a -> FilterChip(selected = cfg.aspect == a, onClick = { cfg = cfg.copy(aspect = a) }, label = { Text(a.label) }) }
                }
                Text("Frame rate")
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    FPS_OPTIONS.forEach { f -> FilterChip(selected = cfg.fps == f, onClick = { cfg = cfg.copy(fps = f) }, label = { Text("$f") }) }
                }
                Text("Quality")
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Quality.values().forEach { q -> FilterChip(selected = cfg.quality == q, onClick = { cfg = cfg.copy(quality = q) }, label = { Text(q.label) }) }
                }
                Text("${cfg.label}  ·  ${"%.1f".format(cfg.videoBitrate / 1e6)} Mbps  ·  H.264 + AAC")
                warning?.let { Text(it) }
            }
        },
        confirmButton = { TextButton(onClick = { onStart(cfg) }) { Text("Export") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** Progress / result of the running or last export. */
@Composable
fun ExportStatus(onDismissResult: () -> Unit) {
    val ctx = LocalContext.current
    if (ExportController.running) {
        AlertDialog(
            onDismissRequest = {},
            title = { Text("Exporting…") },
            text = {
                Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(ExportController.phase)
                    LinearProgressIndicator(progress = { ExportController.progress }, modifier = Modifier.fillMaxWidth())
                    Text("${(ExportController.progress * 100).toInt()}%" +
                        if (ExportController.renderFps > 0) "  ·  ${"%.1f".format(ExportController.renderFps)} frames/s" else "")
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    ctx.startService(Intent(ctx, ExportService::class.java).setAction(ExportService.ACTION_CANCEL))
                }) { Text("Cancel") }
            },
        )
        return
    }
    val uri = ExportController.resultUri
    val err = ExportController.error
    if (uri != null || err != null) {
        AlertDialog(
            onDismissRequest = onDismissResult,
            title = { Text(if (uri != null) "Export complete" else "Export failed") },
            text = { Text(if (uri != null) "Saved to Movies/AudioVisualizer." else err ?: "") },
            confirmButton = {
                if (uri != null) {
                    TextButton(onClick = {
                        ctx.startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(uri, "video/mp4").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
                    }) { Text("Play") }
                } else {
                    TextButton(onClick = onDismissResult) { Text("OK") }
                }
            },
            dismissButton = {
                if (uri != null) {
                    TextButton(onClick = {
                        ctx.startActivity(Intent.createChooser(
                            Intent(Intent.ACTION_SEND).setType("video/mp4").putExtra(Intent.EXTRA_STREAM, uri)
                                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), "Share video"))
                    }) { Text("Share") }
                }
            },
        )
    }
}
