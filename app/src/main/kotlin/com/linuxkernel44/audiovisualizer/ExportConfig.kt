package com.linuxkernel44.audiovisualizer

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat

enum class Aspect(val label: String) { Landscape("16:9"), Portrait("9:16"), Square("1:1") }
enum class Quality(val label: String, val bitsPerPixel: Double) {
    Standard("Standard", 0.07), High("High", 0.10), Max("Max", 0.15)
}

/** User-facing export choices; resolves to concrete pixel dimensions and a bitrate. */
data class ExportConfig(
    val shortSide: Int = 1080,
    val aspect: Aspect = Aspect.Landscape,
    val fps: Int = 30,
    val quality: Quality = Quality.High,
) {
    val width: Int get() = when (aspect) {
        Aspect.Landscape -> shortSide * 16 / 9 / 4 * 4
        Aspect.Portrait -> shortSide
        Aspect.Square -> shortSide
    }
    val height: Int get() = when (aspect) {
        Aspect.Landscape -> shortSide
        Aspect.Portrait -> shortSide * 16 / 9 / 2 * 2
        Aspect.Square -> shortSide
    }
    val videoBitrate: Int get() = (width.toLong() * height * fps * quality.bitsPerPixel).toLong().coerceIn(1_000_000L, 200_000_000L).toInt()
    val label: String get() = "${width}×$height @ $fps fps"

    /** Returns a warning if the device's H.264 encoder may not handle this, or null if it should. */
    fun capabilityWarning(): String? {
        return try {
            val list = MediaCodecList(MediaCodecList.REGULAR_CODECS)
            val encoders = list.codecInfos.filter { it.isEncoder && it.supportedTypes.any { t -> t.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) } }
            if (encoders.isEmpty()) return "No H.264 encoder found on this device."
            val ok = encoders.any { info ->
                val v = info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities
                v != null && v.isSizeSupported(width, height) && v.areSizeAndRateSupported(width, height, fps.toDouble())
            }
            if (ok) null else "This device's H.264 encoder does not advertise $label; the export may fail or be slow."
        } catch (e: Exception) {
            null
        }
    }
}
