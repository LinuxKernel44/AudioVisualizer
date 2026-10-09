package com.linuxkernel44.audiovisualizer

import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.InputChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
fun SettingsDialog(vm: PlayerViewModel, onDismiss: () -> Unit) {
    val ctx = LocalContext.current
    var newName by remember { mutableStateOf("") }
    var tokenInput by remember { mutableStateOf("") }
    val importPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.importPresets(ctx, uri)
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Settings") },
        text = {
            Column(Modifier.heightIn(max = 520.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Battery saver (30 fps, lower resolution)", Modifier.weight(1f).padding(top = 12.dp))
                    Switch(vm.batterySaver, { vm.setBatterySaverEnabled(it) })
                }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Adapt resolution to keep the frame rate", Modifier.weight(1f).padding(top = 12.dp))
                    Switch(vm.autoResolution, { vm.setAutoResolutionEnabled(it) })
                }
                if (vm.recents.isNotEmpty()) {
                    HorizontalDivider()
                    Text("Recent tracks")
                    vm.recents.forEach { r ->
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                            TextButton(onClick = { vm.open(android.net.Uri.parse(r.uri)); onDismiss() }, modifier = Modifier.weight(1f)) {
                                Text(r.name, maxLines = 1)
                            }
                            TextButton(onClick = { vm.forgetRecent(r.uri) }) { Text("✕") }
                        }
                    }
                }
                HorizontalDivider()
                Text("Presets")
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    vm.presets.forEach { p ->
                        if (p.source == Preset.Source.User) {
                            InputChip(
                                selected = false, onClick = { vm.applyPreset(p) }, label = { Text("★ ${p.name}") },
                                trailingIcon = {
                                    Text("✕", Modifier.clickable { vm.deletePreset(p.name) }.padding(horizontal = 4.dp))
                                },
                            )
                        } else {
                            AssistChip(onClick = { vm.applyPreset(p) }, label = { Text(p.name + if (p.source == Preset.Source.Downloaded) " ↓" else "") })
                        }
                    }
                }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(newName, { newName = it }, label = { Text("Save current as…") }, singleLine = true, modifier = Modifier.weight(1f))
                    TextButton(onClick = { vm.savePreset(newName); newName = "" }, enabled = newName.isNotBlank()) { Text("Save") }
                }
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = { importPicker.launch(arrayOf("application/json", "text/*", "*/*")) }) { Text("Import") }
                    TextButton(onClick = {
                        ctx.startActivity(Intent.createChooser(
                            Intent(Intent.ACTION_SEND).setType("application/json").putExtra(Intent.EXTRA_TEXT, vm.exportPresetsJson()), "Share presets"))
                    }) { Text("Export") }
                }
                HorizontalDivider()
                Text("Preset updates (GitHub Releases)")
                OutlinedTextField(
                    tokenInput, { tokenInput = it }, label = { Text(if (vm.hasToken) "Token saved — enter a new one to replace" else "GitHub token (read-only)") },
                    singleLine = true, visualTransformation = PasswordVisualTransformation(), modifier = Modifier.fillMaxWidth(),
                )
                // Wrapping row: three text buttons do not fit side by side in a dialog, and a plain Row
                // squeezed the last one into a one-letter-wide column.
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = { vm.saveToken(tokenInput); tokenInput = "" }, enabled = tokenInput.isNotBlank()) { Text("Save token") }
                    if (vm.hasToken) TextButton(onClick = { vm.clearToken() }) { Text("Forget token") }
                    TextButton(onClick = { vm.checkPresetUpdates() }, enabled = vm.hasToken && !vm.updating) { Text("Check for updates", maxLines = 1) }
                }
                if (vm.updateStatus.isNotEmpty()) Text(vm.updateStatus)
                HorizontalDivider()
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Analysis cache: ${"%.1f".format(vm.cacheBytes / 1e6)} MB", Modifier.padding(top = 12.dp))
                    TextButton(onClick = { vm.clearCache() }) { Text("Clear") }
                }
                Text("AudioVisualizer ${BuildConfigInfo.version(ctx)} · engine ${NativeEngine.version()}")
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } },
    )
}

object BuildConfigInfo {
    fun version(ctx: android.content.Context): String =
        try { ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: "?" } catch (e: Exception) { "?" }
}
