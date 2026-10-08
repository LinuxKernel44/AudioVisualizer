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

    companion object {
        init {
            System.loadLibrary("avengine")
        }

        @JvmStatic external fun version(): String
    }
}
