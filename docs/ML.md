# Beat / downbeat model (Beat This!)

The classical DSP stage always runs. When the ONNX model is available, a second pass replaces the
beat grid, tempo curve and meter with the model's output **if it looks trustworthy**.

## Pipeline (all native, streaming)
`Decoder` -> mono mix -> `Resampler` (windowed sinc) to 22.05 kHz -> `LogMel` (128 bands, 50 fps,
a C++ port of torchaudio's `MelSpectrogram` + `log1p(1000 x)`) -> `ChunkedBeatInference` (chunks of
1500 frames, 6-frame borders, "keep first", identical to the reference `split_piece` /
`aggregate_prediction`) -> `OnnxBeatModel` (ONNX Runtime CPU) -> `postprocessMinimal` (peak picking,
de-duplication, downbeats snapped to beats) -> `applyMlBeats` (Timeline fields).

## Validation
* `engine/tests/data/ref_*.inc` hold reference values produced by the official Python implementation
  (`tools/ml/gen_references.py`): the C++ log-mel matches torchaudio to ~1e-3 and the post-processing
  matches the official minimal post-processor exactly.
* The exported ONNX graph matches PyTorch to ~3e-6 for several sequence lengths
  (`tools/ml/export_beat_this.py`).
* End to end on a Galaxy S10, the beats from the app equal those from the official Python code on the
  same file (differences of at most one frame).

## Quality gates
Beat This! was trained on real music. On a synthetic click track (sine kicks + noise hats) it locks
onto the hats with irregular spacing, while the DSP tracker is exact. `applyMlBeats` therefore rejects
a model result whose inter-beat intervals are not steady (fewer than 70 % within 15 % of the median) or
whose tempo is implausible, keeping the DSP rhythm. A second gate (`beatAlignmentScore`) requires the model grid to sit on the audible pulse (low/mid
onset flux) at least 60 % as well as the DSP grid: a regular but misplaced or slightly mis-tempoed
grid fails it. The summary reports which source was used
(`beatSource`: `ml` / `dsp`).

## Cost and limits
About 10 s of inference per 40 s of audio on a Galaxy S10 CPU (2.1 M parameters, 2 threads). Results are
cached per file and per model, so this happens once. A 1500-frame chunk makes the attention layers
allocate on the order of 1.4 GB transiently (measured on a Galaxy S10), so the ONNX Runtime arena is
disabled and the model is only run for tracks up to 15 minutes; longer tracks keep the DSP rhythm. The DSP stage is
decode bound: a one-hour MP3 analyses in about a minute on the S10 (MP3 frames are packed several
per codec buffer), AAC takes about four to five minutes; memory stays flat (~170 MB).

## Licence
Code and published weights are MIT-licensed. The authors note that some training material is under
limited licences, so whether the weights may be used in a given product is the integrator's judgement.
To swap the model, export another checkpoint with `tools/ml/export_beat_this.py` and replace the asset;
an empty/missing asset simply disables the ML pass.
