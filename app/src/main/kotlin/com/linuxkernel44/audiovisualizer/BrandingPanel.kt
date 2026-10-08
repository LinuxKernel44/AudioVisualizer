package com.linuxkernel44.audiovisualizer

import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BrandingPanel(b: Branding, onChange: (Branding) -> Unit, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    fun keep(uri: android.net.Uri) {
        try {
            ctx.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (_: SecurityException) {
        }
    }
    val pickLogo = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) { keep(uri); onChange(b.copy(logoUri = uri.toString(), enabled = true)) }
    }
    val pickBg = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) { keep(uri); onChange(b.copy(backgroundUri = uri.toString(), enabled = true)) }
    }
    Card(
        modifier = modifier.fillMaxWidth().padding(horizontal = 12.dp),
        colors = CardDefaults.cardColors(containerColor = Color(0xE6101018)),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Title, logo and background", color = Color.White, modifier = Modifier.padding(top = 12.dp))
                Switch(checked = b.enabled, onCheckedChange = { onChange(b.copy(enabled = it)) })
            }
            OutlinedTextField(value = b.title, onValueChange = { onChange(b.copy(title = it)) }, label = { Text("Title") }, singleLine = true, modifier = Modifier.fillMaxWidth())
            OutlinedTextField(value = b.artist, onValueChange = { onChange(b.copy(artist = it)) }, label = { Text("Artist") }, singleLine = true, modifier = Modifier.fillMaxWidth())
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                TitlePosition.values().forEach { p -> FilterChip(selected = b.position == p, onClick = { onChange(b.copy(position = p)) }, label = { Text(p.label) }) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = { pickLogo.launch(arrayOf("image/*")) }) { Text(if (b.logoUri == null) "Add logo" else "Change logo") }
                if (b.logoUri != null) TextButton(onClick = { onChange(b.copy(logoUri = null)) }) { Text("Remove") }
                TextButton(onClick = { pickBg.launch(arrayOf("image/*")) }) { Text(if (b.backgroundUri == null) "Add background" else "Change background") }
                if (b.backgroundUri != null) TextButton(onClick = { onChange(b.copy(backgroundUri = null)) }) { Text("Remove") }
            }
            if (b.backgroundUri != null) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Background", color = Color.White, modifier = Modifier.padding(top = 12.dp).weight(0.9f))
                    Slider(value = b.backgroundOpacity, onValueChange = { onChange(b.copy(backgroundOpacity = it)) }, valueRange = 0f..1f, modifier = Modifier.weight(2f))
                }
            }
        }
    }
}
