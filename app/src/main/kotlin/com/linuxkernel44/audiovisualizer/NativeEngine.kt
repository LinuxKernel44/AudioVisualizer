package com.linuxkernel44.audiovisualizer

import android.view.Surface

/** Thin JNI facade over the native engine. All real work happens in C++. */
class NativeEngine {
    @Volatile private var handle: Long = create()

    /** Calls [f] with the native handle, or returns [default] once the engine has been released. */
    private inline fun <T> h(default: T, f: (Long) -> T): T {
        val hd = handle
        return if (hd != 0L) f(hd) else default
    }

    fun load(fd: Int): String? = h("The engine is closed") { load(it, fd) }
    fun play(): Boolean = h(false) { play(it) }
    fun pause(): Boolean = h(false) { pause(it) }
    fun seekTo(ms: Long): Boolean = h(false) { seekTo(it, ms) }
    fun positionMs(): Long = h(0L) { positionMs(it) }
    fun durationMs(): Long = h(0L) { durationMs(it) }
    fun isPlaying(): Boolean = h(false) { isPlaying(it) }
    fun isFinished(): Boolean = h(false) { isFinished(it) }
    fun frameMs(): Float = h(0f) { frameMs(it) }
    fun setSurface(surface: Surface?) = h(Unit) { setSurface(it, surface) }
    fun setControls(c: Controls) = h(Unit) {
        setControls(it, c.sensitivity, c.gainBass, c.gainMid, c.gainTreble, c.smoothing, c.palette, c.beatFlash, c.bloom)
    }
    fun setTargetFps(fps: Float) = h(Unit) { setTargetFps(it, fps) }
    fun setThermalLevel(level: Int) = h(Unit) { setThermalLevel(it, level) }
    fun setAutoScale(on: Boolean) = h(Unit) { setAutoScale(it, on) }
    fun setPowerSaver(on: Boolean) = h(Unit) { setPowerSaver(it, on) }
    /** [frameMs, gpuMs, renderScale, targetFps, audioLatencyMs] */
    fun perf(): FloatArray = h(FloatArray(5)) { perf(it) }
    fun setScene(index: Int) = h(Unit) { setScene(it, index) }
    fun sceneIndex(): Int = h(-1) { sceneIndex(it) }
    fun setCacheDir(dir: String) = h(Unit) { setCacheDir(it, dir) }
    fun setBeatModel(path: String) = h(Unit) { setBeatModel(it, path) }
    fun setOverlay(l: Layer?) = h(Unit) { setOverlay(it, l?.rgba, l?.width ?: 0, l?.height ?: 0) }
    fun setBackground(l: Layer?) = h(Unit) { setBackground(it, l?.rgba, l?.width ?: 0, l?.height ?: 0) }
    fun setBrandingOpacity(overlay: Float, background: Float) = h(Unit) { setBrandingOpacity(it, overlay, background) }
    fun analysisState(): Int = h(0) { analysisState(it) }
    fun analysisProgress(): Float = h(0f) { analysisProgress(it) }
    fun analysisFromCache(): Boolean = h(false) { analysisFromCache(it) }
    fun analysisError(): String = h("") { analysisError(it) }
    fun summaryJson(): String = h("") { summaryJson(it) }

    /** Destroys the native engine. No other call may be in flight; later calls are no-ops. */
    @Synchronized
    fun release() {
        val hd = handle
        if (hd != 0L) {
            handle = 0L
            destroy(hd)
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
    private external fun setPowerSaver(h: Long, on: Boolean)
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
