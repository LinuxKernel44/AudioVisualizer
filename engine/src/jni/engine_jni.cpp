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

JNI(void, setScene)(JNIEnv*, jobject, jlong h, jint index) { from(h)->setScene(index); }
JNI(jint, sceneIndex)(JNIEnv*, jobject, jlong h) { return from(h)->sceneIndex(); }

JNI(void, setControls)(JNIEnv*, jobject, jlong h, jfloat sensitivity, jfloat gainBass, jfloat gainMid,
                       jfloat gainTreble, jfloat smoothing, jint palette, jfloat beatFlash, jfloat bloom) {
    av::RenderSettings s;
    s.sensitivity = sensitivity;
    s.gainBass = gainBass;
    s.gainMid = gainMid;
    s.gainTreble = gainTreble;
    s.smoothing = smoothing;
    s.palette = palette;
    s.beatFlash = beatFlash;
    s.bloom = bloom;
    from(h)->setSettings(s);
}

// ---------------------------------------------------------------------------------------------
// Export
#include "av/export/export_job.h"

#define JNIX(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_linuxkernel44_audiovisualizer_NativeExport_##name

namespace {
av::ExportJob* job(jlong h) { return reinterpret_cast<av::ExportJob*>(h); }
}  // namespace

JNIX(jlong, start)(JNIEnv* env, jobject, jint audioFd, jint outFd, jint width, jint height, jint fps,
                   jint videoBitrate, jint audioBitrate, jint scene, jfloat renderScale, jfloat sensitivity,
                   jfloat gainBass, jfloat gainMid, jfloat gainTreble, jfloat smoothing, jint palette,
                   jfloat beatFlash, jfloat bloom, jstring cacheDir) {
    av::ExportParams p;
    p.audioFd = audioFd;
    p.outFd = outFd;
    p.width = width;
    p.height = height;
    p.fps = fps;
    p.videoBitrate = videoBitrate;
    p.audioBitrate = audioBitrate;
    p.scene = scene;
    p.renderScale = renderScale;
    p.settings.sensitivity = sensitivity;
    p.settings.gainBass = gainBass;
    p.settings.gainMid = gainMid;
    p.settings.gainTreble = gainTreble;
    p.settings.smoothing = smoothing;
    p.settings.palette = palette;
    p.settings.beatFlash = beatFlash;
    p.settings.bloom = bloom;
    const char* c = env->GetStringUTFChars(cacheDir, nullptr);
    p.cacheDir = c;
    env->ReleaseStringUTFChars(cacheDir, c);
    auto* j = new av::ExportJob(std::move(p));
    j->start();
    return reinterpret_cast<jlong>(j);
}
JNIX(jint, state)(JNIEnv*, jobject, jlong h) { return static_cast<jint>(job(h)->state()); }
JNIX(jfloat, progress)(JNIEnv*, jobject, jlong h) { return job(h)->progress(); }
JNIX(jfloat, renderFps)(JNIEnv*, jobject, jlong h) { return job(h)->renderFps(); }
JNIX(jstring, error)(JNIEnv* env, jobject, jlong h) { return env->NewStringUTF(job(h)->error().c_str()); }
JNIX(jstring, phase)(JNIEnv* env, jobject, jlong h) { return env->NewStringUTF(job(h)->phase().c_str()); }
JNIX(void, cancel)(JNIEnv*, jobject, jlong h) { job(h)->cancel(); }
JNIX(void, release)(JNIEnv*, jobject, jlong h) { delete job(h); }
