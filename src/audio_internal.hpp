// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// audio_internal.hpp — shared DSP core for PCM capture back-ends (private).
//
// PcmAudioSource holds a lock-guarded ring of recent mono samples and turns
// them into the Shadertoy 512x2 audio texture (FFT spectrum + waveform) using
// the same byte mapping Web Audio's AnalyserNode produces, so shaders authored
// against Shadertoy behave the same here.  A platform back-end (e.g. PipeWire)
// subclasses it and calls Push() from its capture callback; Fill() runs the
// analysis on the render thread.

#pragma once

#include "shadertoy/audio.hpp"

#include <shadertoy/config.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace shadertoy {

class PcmAudioSource : public AudioSource {
 public:
  PcmAudioSource();

  // Compute FFT + waveform from the most recent samples.  Thread-safe.
  [[nodiscard]] bool Fill(unsigned char* dst) noexcept override;

  // No-op lifecycle; capture back-ends override these.
  [[nodiscard]] bool Start() override { return true; }
  void Stop() noexcept override {}

 protected:
  // Append @p n mono samples (thread-safe; called from the capture thread).
  void Push(const float* samples, std::size_t n) noexcept;

 private:
  static constexpr int kFftSize = 1024;  // 512 frequency bins
  static constexpr std::size_t kRingSize = 1u
                                           << 12;  // recent-sample ring (pow2)

  std::mutex mu_;            // guards the ring (capture thread vs Fill)
  std::vector<float> ring_;  // kRingSize mono samples, circular
  std::size_t write_ = 0;    // total samples ever written (monotonic)

  // Analysis cache, guarded by analysis_mu_ so concurrent Fill() callers share
  // one FFT: recomputed only when write_ advances past analyzed_at_.
  std::mutex analysis_mu_;
  std::vector<float>
      smooth_;  // per-bin time-smoothed magnitude (linear domain)
  std::array<unsigned char, kAudioTexBytes> cached_{};  // last 512x2 texture
  std::size_t analyzed_at_ = 0;  // write_ value the cache was computed at
  bool analyzed_once_ = false;   // cache holds a valid texture
};

// Per-back-end factories (each compiled only with its dependency).
// MakeMicSource in audio.cpp selects between them; the returned source is
// stopped.
#if SHADERTOY_HAVE_PIPEWIRE
[[nodiscard]] std::unique_ptr<AudioSource> MakePipeWireMicSource();
#endif
#if SHADERTOY_HAVE_ALSA
[[nodiscard]] std::unique_ptr<AudioSource> MakeAlsaMicSource();
#endif

}  // namespace shadertoy
