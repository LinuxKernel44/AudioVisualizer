package com.linuxkernel44.audiovisualizer

/** Thin JNI facade over the native engine. All real work happens in C++. */
object NativeEngine {
    init {
        System.loadLibrary("avengine")
    }

    external fun version(): String
}
