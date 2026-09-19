# GuitarFX TODO

This document tracks outstanding tasks and improvements for the GuitarFX project.

## DSP (Digital Signal Processing)

### Correctness & Stability
- [ ] Fix NoiseGateEffect hold implementation: Currently sets hold parameter but doesn't use it in Process()—gate is instantaneous without hold time. Implement proper hold logic to maintain gate state for specified duration after signal drops below threshold.

### Testing
- [ ] Add unit tests for edge cases:
  - Extreme parameter values (e.g., max/min gains, Q factors, feedback levels)
  - Various buffer sizes (32, 64, 128, 256, 512, 1024, 2048, 4096 samples)
  - Sample rate transitions (44.1kHz, 48kHz, 88.2kHz, 96kHz)
  - Signal integrity checks (no NaN/Inf, silence detection, frequency response validation)

### Performance Optimization
- [ ] Implement SIMD optimizations in hot paths:
  - Vectorize ParametricEQ biquad processing loops using AVX intrinsics
  - Optimize buffer clearing/copying in SignalGraphExecutor with SIMD
  - Expand FFT SIMD usage beyond RealtimeConvolver to other frequency-domain effects
- [ ] Add performance benchmarking:
  - Profile CPU usage against <30% target for typical presets
  - Measure latency for convolution effects (limit IR length to maintain <10ms total latency)
  - Implement basic profiling infrastructure (e.g., chrono timers) for ongoing monitoring

### Other
- [ ] Review convolution resampling: Current linear interpolation may introduce aliasing—consider cubic interpolation for better quality at large sample rate mismatches.

---

## Planned Features

### Multi-Model NAM Blend Effect (`amp_nam_blend`)

Shipped: see [features.md §7](features.md) and [fx-library.md](fx-library.md). Still open:

- **Weight interpolation** (R&D, behind a flag if ever): morph the weights of two captures of one architecture instead of mixing their outputs. Only valid for identical architectures, not real-time safe to update live, and nonlinear activations make the result unpredictable, so output mixing stays the default.
- **2-D surfaces**: the engine already matches several captured parameters at once (nearest two by distance), but a Gain × Tone grid would crossfade better with bilinear weights over the four surrounding captures.
- **Per-capture level matching** for models without calibration metadata, so a crossfade does not also change the level.


# Facotry Presets general notes
- Preset archive export from a presets folder should include information on the preset subfolder structure, import should (optionally) recreate that subfolder structure. Factory preset import should create/update the folder structure

- Hash the factory preset archive to decide if it has changed on startup, if it has changed re-import it. New factory presets should be added, existing presets should be updated

- Factory preset import should change the preset category to Factory in import

- Factory preset import should persits the imported persets and folder structure in the users preset structure, under a top level Factory Pesets folder

- Factory presets should Import to a Factory Presets folder with a folder structure, it should also import to our resource library with resource item metadata. Like a partial resource library export/import.