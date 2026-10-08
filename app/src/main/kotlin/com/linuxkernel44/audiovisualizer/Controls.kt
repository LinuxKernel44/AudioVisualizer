package com.linuxkernel44.audiovisualizer

import android.content.SharedPreferences

/** Live look-and-feel controls; mirrors the native RenderSettings. */
data class Controls(
    val sensitivity: Float = 1f,
    val gainBass: Float = 0f,
    val gainMid: Float = 0f,
    val gainTreble: Float = 0f,
    val smoothing: Float = 0.3f,
    val palette: Int = -1,
    val beatFlash: Float = 1f,
    val bloom: Float = 1f,
) {
    fun save(p: SharedPreferences) = p.edit()
        .putFloat("sensitivity", sensitivity).putFloat("gainBass", gainBass).putFloat("gainMid", gainMid)
        .putFloat("gainTreble", gainTreble).putFloat("smoothing", smoothing).putInt("palette", palette)
        .putFloat("beatFlash", beatFlash).putFloat("bloom", bloom).apply()

    companion object {
        fun load(p: SharedPreferences) = Controls(
            p.getFloat("sensitivity", 1f), p.getFloat("gainBass", 0f), p.getFloat("gainMid", 0f),
            p.getFloat("gainTreble", 0f), p.getFloat("smoothing", 0.3f), p.getInt("palette", -1),
            p.getFloat("beatFlash", 1f), p.getFloat("bloom", 1f),
        )
    }
}

val PALETTE_NAMES = listOf("Neon", "Ember", "Aurora", "Synthwave", "Ice", "Acid")
