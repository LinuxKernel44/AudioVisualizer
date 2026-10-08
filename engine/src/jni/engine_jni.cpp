#include <android/native_window_jni.h>
#include <jni.h>

#include <string>

#include "av/engine.h"
#include "av/version.h"

namespace {
av::Engine* from(jlong h) { return reinterpret_cast<av::Engine*>(h); }
}  // namespace

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_linuxkernel44_audiovisualizer_NativeEngine_##name

JNI(jstring, version)(JNIEnv* env, jobject) { return env->NewStringUTF(av::engineVersion()); }
JNI(jlong, create)(JNIEnv*, jobject) { return reinterpret_cast<jlong>(new av::Engine()); }
JNI(void, destroy)(JNIEnv*, jobject, jlong h) { delete from(h); }

/// Returns null on success, otherwise a user-presentable error message.
JNI(jstring, load)(JNIEnv* env, jobject, jlong h, jint fd) {
    std::string err;
    if (from(h)->load(fd, &err)) return nullptr;
    return env->NewStringUTF(err.c_str());
}

JNI(jboolean, play)(JNIEnv*, jobject, jlong h) { return from(h)->play(); }
JNI(jboolean, pause)(JNIEnv*, jobject, jlong h) { return from(h)->pause(); }
JNI(jboolean, seekTo)(JNIEnv*, jobject, jlong h, jlong ms) { return from(h)->seek(ms); }
JNI(jlong, positionMs)(JNIEnv*, jobject, jlong h) { return from(h)->positionMs(); }
JNI(jlong, durationMs)(JNIEnv*, jobject, jlong h) { return from(h)->durationMs(); }
JNI(jboolean, isPlaying)(JNIEnv*, jobject, jlong h) { return from(h)->isPlaying(); }
JNI(jboolean, isFinished)(JNIEnv*, jobject, jlong h) { return from(h)->finished(); }
JNI(jfloat, frameMs)(JNIEnv*, jobject, jlong h) { return from(h)->frameMs(); }

JNI(void, setSurface)(JNIEnv* env, jobject, jlong h, jobject surface) {
    ANativeWindow* w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    from(h)->setSurface(w);  // the renderer takes its own reference
    if (w) ANativeWindow_release(w);
}

JNI(void, setCacheDir)(JNIEnv* env, jobject, jlong h, jstring dir) {
    const char* c = env->GetStringUTFChars(dir, nullptr);
    from(h)->setCacheDir(c);
    env->ReleaseStringUTFChars(dir, c);
}
JNI(jint, analysisState)(JNIEnv*, jobject, jlong h) { return from(h)->analysisState(); }
JNI(jfloat, analysisProgress)(JNIEnv*, jobject, jlong h) { return from(h)->analysisProgress(); }
JNI(jboolean, analysisFromCache)(JNIEnv*, jobject, jlong h) { return from(h)->analysisFromCache(); }
JNI(jstring, analysisError)(JNIEnv* env, jobject, jlong h) {
    return env->NewStringUTF(from(h)->analysisError().c_str());
}
JNI(jstring, summaryJson)(JNIEnv* env, jobject, jlong h) {
    return env->NewStringUTF(from(h)->summaryJson().c_str());
}
