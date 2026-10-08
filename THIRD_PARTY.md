# Third-party software
| Name | License | Use |
|---|---|---|
| GoogleTest | BSD-3-Clause | host unit tests (fetched at configure time) |
| PFFFT (marton78 fork) | BSD-like (FFTPACK) | FFT; vendored in `third_party/pffft` (see LICENSE.txt, UPSTREAM_COMMIT) |
| ONNX Runtime 1.30 (Android AAR) | MIT | runs the beat model (`libonnxruntime.so`, headers unpacked at build time) |
| Beat This! "small0" weights + code (CPJKU) | MIT | beat/downbeat model, exported to ONNX (`app/src/main/assets/models/beat_this_small0.onnx`, SHA-256 `8213e7c4…cd805`); see `docs/ML.md` for the training-data caveat |
