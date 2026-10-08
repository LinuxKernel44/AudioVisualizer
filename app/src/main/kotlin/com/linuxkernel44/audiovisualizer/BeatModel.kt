package com.linuxkernel44.audiovisualizer

import android.content.Context
import java.io.File

/**
 * The Beat This! ("small0") beat/downbeat model, exported to ONNX by tools/ml/export_beat_this.py.
 * It ships as an app asset and is copied to private storage once so ONNX Runtime can open it.
 * Returns an empty path if the asset is missing, in which case analysis uses the DSP rhythm only.
 */
object BeatModel {
    private const val ASSET = "models/beat_this_small0.onnx"

    fun path(ctx: Context): String {
        val dir = File(ctx.filesDir, "models").apply { mkdirs() }
        val out = File(dir, "beat_this_small0.onnx")
        return try {
            val size = ctx.assets.openFd(ASSET).use { it.length }
            if (!out.exists() || out.length() != size) {
                ctx.assets.open(ASSET).use { input ->
                    File(dir, "tmp.onnx").also { tmp ->
                        tmp.outputStream().use { input.copyTo(it) }
                        tmp.renameTo(out)
                    }
                }
            }
            out.absolutePath
        } catch (e: Exception) {
            ""
        }
    }
}
