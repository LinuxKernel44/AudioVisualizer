# JNI entry points are looked up by name from native code.
-keep class com.linuxkernel44.audiovisualizer.NativeEngine { *; }
-keep class com.linuxkernel44.audiovisualizer.NativeExport { *; }
# ONNX Runtime's Java layer is not used (the model runs through the C++ API).
-dontwarn ai.onnxruntime.**
