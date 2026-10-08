#include <jni.h>

#include "av/version.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_linuxkernel44_audiovisualizer_NativeEngine_version(JNIEnv* env, jobject /*thiz*/) {
    return env->NewStringUTF(av::engineVersion());
}
