#include <android/native_window_jni.h>
#include <jni.h>

#include <string>

#include "av/engine.h"
#include "av/version.h"

namespace {
av::Engine* from(jlong h) { return reinterpret_cast<av::Engine*>(h); }
}  // namespace

#define JNI(ret, name) \
    extern "C" JNIEXPORT ret JNICALL Java_com_linuxkernel44_audiovisualizer_NativeEngine_##name

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

JNI(void, setBeatModel)(JNIEnv* env, jobject, jlong h, jstring path) {
    const char* c = env->GetStringUTFChars(path, nullptr);
    from(h)->setBeatModel(c);
    env->ReleaseStringUTFChars(path, c);
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

JNI(void, setControls)(JNIEnv*, jobject, jlong h, jfloat sensitivity, jfloat gainBass,
                       jfloat gainMid, jfloat gainTreble, jfloat smoothing, jint palette,
                       jfloat beatFlash, jfloat bloom) {
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

namespace {
std::vector<uint8_t> bytesOf(JNIEnv* env, jbyteArray a) {
    std::vector<uint8_t> v;
    if (!a) return v;
    const jsize n = env->GetArrayLength(a);
    v.resize(static_cast<size_t>(n));
    env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(v.data()));
    return v;
}
}  // namespace

JNI(void, setOverlay)(JNIEnv* env, jobject, jlong h, jbyteArray rgba, jint w, jint ht) {
    from(h)->setOverlayImage(bytesOf(env, rgba), static_cast<uint32_t>(w),
                             static_cast<uint32_t>(ht));
}
JNI(void, setBackground)(JNIEnv* env, jobject, jlong h, jbyteArray rgba, jint w, jint ht) {
    from(h)->setBackgroundImage(bytesOf(env, rgba), static_cast<uint32_t>(w),
                                static_cast<uint32_t>(ht));
}
JNI(void, setBrandingOpacity)(JNIEnv*, jobject, jlong h, jfloat overlay, jfloat background) {
    from(h)->setBrandingOpacity(overlay, background);
}
JNI(void, setTargetFps)(JNIEnv*, jobject, jlong h, jfloat fps) { from(h)->setTargetFps(fps); }
JNI(void, setThermalLevel)(JNIEnv*, jobject, jlong h, jint level) {
    from(h)->setThermalLevel(level);
}
JNI(void, setAutoScale)(JNIEnv*, jobject, jlong h, jboolean on) { from(h)->setAutoScale(on); }
JNI(void, setPowerSaver)(JNIEnv*, jobject, jlong h, jboolean on) { from(h)->setPowerSaver(on); }
JNI(jfloatArray, perf)(JNIEnv* env, jobject, jlong h) {
    const auto p = from(h)->perf();
    const jfloat v[4] = {p.frameMs, p.gpuMs, p.scale, p.targetFps};
    jfloatArray a = env->NewFloatArray(4);
    env->SetFloatArrayRegion(a, 0, 4, v);
    return a;
}

// ---------------------------------------------------------------------------------------------
// Export
#include "av/export/export_job.h"

#define JNIX(ret, name) \
    extern "C" JNIEXPORT ret JNICALL Java_com_linuxkernel44_audiovisualizer_NativeExport_##name

namespace {
av::ExportJob* job(jlong h) { return reinterpret_cast<av::ExportJob*>(h); }
}  // namespace

JNIX(jlong, start)(JNIEnv* env, jobject, jint audioFd, jint outFd, jint width, jint height,
                   jint fps, jint videoBitrate, jint audioBitrate, jint scene, jfloat renderScale,
                   jfloat sensitivity, jfloat gainBass, jfloat gainMid, jfloat gainTreble,
                   jfloat smoothing, jint palette, jfloat beatFlash, jfloat bloom, jstring cacheDir,
                   jstring modelPath, jbyteArray overlay, jint overlayW, jint overlayH,
                   jbyteArray background, jint backgroundW, jint backgroundH, jfloat overlayOpacity,
                   jfloat backgroundOpacity) {
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
    const char* m = env->GetStringUTFChars(modelPath, nullptr);
    p.modelPath = m;
    env->ReleaseStringUTFChars(modelPath, m);
    p.overlay = bytesOf(env, overlay);
    p.overlayW = static_cast<uint32_t>(overlayW);
    p.overlayH = static_cast<uint32_t>(overlayH);
    p.background = bytesOf(env, background);
    p.backgroundW = static_cast<uint32_t>(backgroundW);
    p.backgroundH = static_cast<uint32_t>(backgroundH);
    p.overlayOpacity = overlayOpacity;
    p.backgroundOpacity = backgroundOpacity;
    auto* j = new av::ExportJob(std::move(p));
    j->start();
    return reinterpret_cast<jlong>(j);
}
JNIX(jint, state)(JNIEnv*, jobject, jlong h) { return static_cast<jint>(job(h)->state()); }
JNIX(jfloat, progress)(JNIEnv*, jobject, jlong h) { return job(h)->progress(); }
JNIX(jfloat, renderFps)(JNIEnv*, jobject, jlong h) { return job(h)->renderFps(); }
JNIX(jstring, error)(JNIEnv* env, jobject, jlong h) {
    return env->NewStringUTF(job(h)->error().c_str());
}
JNIX(jstring, phase)(JNIEnv* env, jobject, jlong h) {
    return env->NewStringUTF(job(h)->phase().c_str());
}
JNIX(void, cancel)(JNIEnv*, jobject, jlong h) { job(h)->cancel(); }
JNIX(void, release)(JNIEnv*, jobject, jlong h) { delete job(h); }
