package com.linuxkernel44.audiovisualizer

/** JNI facade over the native offline exporter. */
object NativeExport {
    init {
        System.loadLibrary("avengine")
    }

    external fun start(
        audioFd: Int, outFd: Int, width: Int, height: Int, fps: Int, videoBitrate: Int, audioBitrate: Int,
        scene: Int, renderScale: Float, sensitivity: Float, gainBass: Float, gainMid: Float, gainTreble: Float,
        smoothing: Float, palette: Int, beatFlash: Float, bloom: Float, cacheDir: String, modelPath: String,
        overlay: ByteArray?, overlayW: Int, overlayH: Int, background: ByteArray?, backgroundW: Int, backgroundH: Int,
        overlayOpacity: Float, backgroundOpacity: Float,
    ): Long

    /** 0 running, 1 done, 2 failed, 3 cancelled. */
    external fun state(h: Long): Int
    external fun progress(h: Long): Float
    external fun renderFps(h: Long): Float
    external fun error(h: Long): String
    external fun phase(h: Long): String
    external fun cancel(h: Long)
    external fun release(h: Long)
}
