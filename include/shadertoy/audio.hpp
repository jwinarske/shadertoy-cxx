// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// audio.hpp — live audio input for Shadertoy "mic"/"musicstream" channels.
//
// A Shadertoy audio channel samples a 512x2 single-channel texture: row 0 is an
// FFT magnitude spectrum, row 1 the raw PCM waveform, each value 0..255.  This
// header declares the back-end-agnostic AudioSource interface the GL renderer
// uploads from, plus MakeMicSource(), which returns a microphone-capture source
// when an audio back-end was compiled in (currently PipeWire,
// SHADERTOY_HAVE_PIPEWIRE) and nullptr otherwise.

#pragma once

#include <memory>

namespace shadertoy {

// Texels per row of the Shadertoy audio texture (512 wide, 2 rows).
inline constexpr int kAudioTexWidth = 512;
// Total bytes written by AudioSource::Fill (row 0 FFT, row 1 waveform).
inline constexpr int kAudioTexBytes = kAudioTexWidth * 2;

// A live audio input bound to an iChannel.  Implementations capture PCM from a
// system device and expose it as the Shadertoy audio texture: the first 512
// bytes are an FFT magnitude spectrum, the next 512 the waveform.
class AudioSource {
 public:
  virtual ~AudioSource() = default;

  // Begin capturing.  Returns false if the device could not be opened (no
  // device, permission denied, back-end unavailable).
  [[nodiscard]] virtual bool Start() = 0;

  // Stop capturing and release the device.  Idempotent.
  virtual void Stop() noexcept = 0;

  // Write the current texture into @p dst (kAudioTexBytes bytes: [0,512) FFT,
  // [512,1024) waveform).  Non-blocking; intended to run once per rendered
  // frame.  Returns false if no data was produced (dst left untouched).
  [[nodiscard]] virtual bool Fill(unsigned char* dst) noexcept = 0;
};

// Create a microphone-capture source.  Returns the PipeWire-backed source when
// the library was built with the PipeWire back-end (SHADERTOY_HAVE_PIPEWIRE),
// otherwise nullptr.  The source is returned stopped; call Start().
[[nodiscard]] std::unique_ptr<AudioSource> MakeMicSource();

}  // namespace shadertoy
