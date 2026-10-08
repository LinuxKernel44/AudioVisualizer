package com.linuxkernel44.audiovisualizer

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private val clusterColors = listOf(
    Color(0xFF7C4DFF), Color(0xFF00B8D4), Color(0xFFFF6E40), Color(0xFF64DD17),
    Color(0xFFFFD600), Color(0xFFF50057), Color(0xFF00E5FF), Color(0xFFB388FF),
)

/** Overview of the track: sections, energy curve, downbeats and the playhead. Tap or drag to seek. */
@Composable
fun TimelineStrip(
    summary: AnalysisSummary,
    positionSec: Float,
    onSeek: (Float) -> Unit,
    modifier: Modifier = Modifier,
) {
    val measurer = rememberTextMeasurer()
    var dragSec by remember { mutableStateOf<Float?>(null) }
    val shown = dragSec ?: positionSec
    val dur = summary.durationSec.coerceAtLeast(0.001f)

    Canvas(
        modifier = modifier
            .fillMaxWidth()
            .height(64.dp)
            .pointerInput(dur) {
                detectTapGestures { o -> onSeek((o.x / size.width * dur).coerceIn(0f, dur)) }
            }
            .pointerInput(dur) {
                detectDragGestures(
                    onDragStart = { o -> dragSec = (o.x / size.width * dur).coerceIn(0f, dur) },
                    onDrag = { change, _ -> dragSec = (change.position.x / size.width * dur).coerceIn(0f, dur) },
                    onDragEnd = { dragSec?.let(onSeek); dragSec = null },
                    onDragCancel = { dragSec = null },
                )
            },
    ) {
        val w = size.width
        val h = size.height
        // Sections
        summary.sections.forEach { s ->
            val c = clusterColors[s.cluster % clusterColors.size]
            drawRect(c.copy(alpha = 0.18f), Offset(s.start / dur * w, 0f), Size((s.end - s.start) / dur * w, h))
            drawRect(c.copy(alpha = 0.9f), Offset(s.start / dur * w, 0f), Size(2f, h))
            val label = measurer.measure(s.label, TextStyle(color = Color.White.copy(alpha = 0.85f), fontSize = 10.sp))
            if ((s.end - s.start) / dur * w > label.size.width + 12f) {
                drawText(label, topLeft = Offset(s.start / dur * w + 6f, 2f))
            }
        }
        // Energy curve
        if (summary.energy.size > 1) {
            val path = Path().apply {
                moveTo(0f, h)
                summary.energy.forEachIndexed { i, e ->
                    lineTo((i + 0.5f) / summary.energy.size.coerceAtLeast(1) * w, h - e * (h - 16f))
                }
                lineTo(w, h)
                close()
            }
            drawPath(path, Color.White.copy(alpha = 0.28f))
        }
        // Downbeats
        summary.downbeats.forEach { t ->
            val x = t / dur * w
            drawLine(Color.White.copy(alpha = 0.25f), Offset(x, h - 8f), Offset(x, h), strokeWidth = 1f)
        }
        // Playhead
        val px = shown / dur * w
        drawLine(Color.White, Offset(px, 0f), Offset(px, h), strokeWidth = 3f)
        drawCircle(Color.White, 6f, Offset(px, h / 2))
        drawRect(Color.White.copy(alpha = 0.3f), Offset.Zero, Size(w, h), style = Stroke(1f))
    }
}
