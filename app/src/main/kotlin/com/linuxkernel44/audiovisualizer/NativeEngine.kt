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
    fun setScene(index: Int) = setScene(handle, index)
    fun sceneIndex(): Int = sceneIndex(handle)
    fun setCacheDir(dir: String) = setCacheDir(handle, dir)
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
    private external fun setScene(h: Long, index: Int)
    private external fun sceneIndex(h: Long): Int
    private external fun setCacheDir(h: Long, dir: String)
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
