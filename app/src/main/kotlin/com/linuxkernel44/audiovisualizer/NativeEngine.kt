package com.linuxkernel44.audiovisualizer

import android.view.Surface

/** Thin JNI facade over the native engine. All real work happens in C++. */
class NativeEngine {
    private var handle: Long = create()

    fun load(fd: Int): String? = load(handle, fd)
    fun play(): Boolean = play(handle)
    fun pause(): Boolean = pause(handle)
    fun seekTo(ms: Long): Boolean = seekTo(handle, ms)
    fun positionMs(): Long = positionMs(handle)
    fun durationMs(): Long = durationMs(handle)
    fun isPlaying(): Boolean = isPlaying(handle)
    fun isFinished(): Boolean = isFinished(handle)
    fun frameMs(): Float = frameMs(handle)
    fun setSurface(surface: Surface?) = setSurface(handle, surface)
    fun setControls(c: Controls) = setControls(handle, c.sensitivity, c.gainBass, c.gainMid, c.gainTreble,
        c.smoothing, c.palette, c.beatFlash, c.bloom)
    fun setTargetFps(fps: Float) = setTargetFps(handle, fps)
    fun setThermalLevel(level: Int) = setThermalLevel(handle, level)
    fun setAutoScale(on: Boolean) = setAutoScale(handle, on)
    /** [frameMs, gpuMs, renderScale, targetFps] */
    fun perf(): FloatArray = perf(handle)
    fun setScene(index: Int) = setScene(handle, index)
    fun sceneIndex(): Int = sceneIndex(handle)
    fun setCacheDir(dir: String) = setCacheDir(handle, dir)
    fun setBeatModel(path: String) = setBeatModel(handle, path)
    fun setOverlay(l: Layer?) = setOverlay(handle, l?.rgba, l?.width ?: 0, l?.height ?: 0)
    fun setBackground(l: Layer?) = setBackground(handle, l?.rgba, l?.width ?: 0, l?.height ?: 0)
    fun setBrandingOpacity(overlay: Float, background: Float) = setBrandingOpacity(handle, overlay, background)
    fun analysisState(): Int = analysisState(handle)
    fun analysisProgress(): Float = analysisProgress(handle)
    fun analysisFromCache(): Boolean = analysisFromCache(handle)
    fun analysisError(): String = analysisError(handle)
    fun summaryJson(): String = summaryJson(handle)

    fun release() {
        if (handle != 0L) {
            destroy(handle)
            handle = 0L
        }
    }

    private external fun create(): Long
    private external fun destroy(h: Long)
    private external fun load(h: Long, fd: Int): String?
    private external fun play(h: Long): Boolean
    private external fun pause(h: Long): Boolean
    private external fun seekTo(h: Long, ms: Long): Boolean
    private external fun positionMs(h: Long): Long
    private external fun durationMs(h: Long): Long
    private external fun isPlaying(h: Long): Boolean
    private external fun isFinished(h: Long): Boolean
    private external fun frameMs(h: Long): Float
    private external fun setSurface(h: Long, surface: Surface?)
    private external fun setControls(h: Long, sensitivity: Float, gainBass: Float, gainMid: Float,
        gainTreble: Float, smoothing: Float, palette: Int, beatFlash: Float, bloom: Float)
    private external fun setTargetFps(h: Long, fps: Float)
    private external fun setThermalLevel(h: Long, level: Int)
    private external fun setAutoScale(h: Long, on: Boolean)
    private external fun perf(h: Long): FloatArray
    private external fun setScene(h: Long, index: Int)
    private external fun sceneIndex(h: Long): Int
    private external fun setCacheDir(h: Long, dir: String)
    private external fun setBeatModel(h: Long, path: String)
    private external fun setOverlay(h: Long, rgba: ByteArray?, w: Int, ht: Int)
    private external fun setBackground(h: Long, rgba: ByteArray?, w: Int, ht: Int)
    private external fun setBrandingOpacity(h: Long, overlay: Float, background: Float)
    private external fun analysisState(h: Long): Int
    private external fun analysisProgress(h: Long): Float
    private external fun analysisFromCache(h: Long): Boolean
    private external fun analysisError(h: Long): String
    private external fun summaryJson(h: Long): String

    companion object {
        init {
            System.loadLibrary("avengine")
        }

        @JvmStatic external fun version(): String
    }
}
