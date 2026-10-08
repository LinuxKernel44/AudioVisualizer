package com.linuxkernel44.audiovisualizer

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ControlsPanel(c: Controls, onChange: (Controls) -> Unit, modifier: Modifier = Modifier) {
    @Composable
    fun row(label: String, value: Float, range: ClosedFloatingPointRange<Float>, fmt: String, set: (Float) -> Controls) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(label, color = Color.White, modifier = Modifier.padding(top = 12.dp).weight(0.9f))
            Slider(value = value, onValueChange = { onChange(set(it)) }, valueRange = range, modifier = Modifier.weight(2f))
            Text(fmt.format(value), color = Color.White, modifier = Modifier.padding(top = 12.dp).weight(0.6f))
        }
    }
    Card(
        modifier = modifier.fillMaxWidth().padding(horizontal = 12.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xCC101018)),
    ) {
        Column(Modifier.padding(12.dp)) {
            row("Sensitivity", c.sensitivity, 0.25f..4f, "%.2f×") { c.copy(sensitivity = it) }
            row("Bass", c.gainBass, -20f..20f, "%+.0f dB") { c.copy(gainBass = it) }
            row("Mid", c.gainMid, -20f..20f, "%+.0f dB") { c.copy(gainMid = it) }
            row("Treble", c.gainTreble, -20f..20f, "%+.0f dB") { c.copy(gainTreble = it) }
            row("Smoothing", c.smoothing, 0f..1f, "%.2f") { c.copy(smoothing = it) }
            row("Beat flash", c.beatFlash, 0f..2f, "%.2f") { c.copy(beatFlash = it) }
            row("Bloom", c.bloom, 0f..2f, "%.2f") { c.copy(bloom = it) }
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                FilterChip(selected = c.palette < 0, onClick = { onChange(c.copy(palette = -1)) }, label = { Text("Auto") })
                PALETTE_NAMES.forEachIndexed { i, n ->
                    FilterChip(selected = c.palette == i, onClick = { onChange(c.copy(palette = i)) }, label = { Text(n) })
                }
            }
            TextButton(onClick = { onChange(Controls()) }) { Text("Reset") }
        }
    }
}
