// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// audio.cpp — back-end-agnostic audio DSP: ring buffer, FFT, and the
// Shadertoy/Web-Audio byte mapping shared by every capture back-end.

#include "audio_internal.hpp"

#include <shadertoy/config.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace shadertoy {

namespace {

// In-place iterative radix-2 Cooley–Tukey FFT (N a power of two).
void Fft(std::vector<float>& re, std::vector<float>& im) {
  const std::size_t n = re.size();
  // Bit-reversal permutation.
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const float ang = -2.0f * 3.14159265358979323846f / static_cast<float>(len);
    const float wr = std::cos(ang);
    const float wi = std::sin(ang);
    for (std::size_t i = 0; i < n; i += len) {
      float cur_r = 1.0f, cur_i = 0.0f;
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::size_t a = i + k;
        const std::size_t b = i + k + len / 2;
        const float tr = cur_r * re[b] - cur_i * im[b];
        const float ti = cur_r * im[b] + cur_i * re[b];
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
        const float nr = cur_r * wr - cur_i * wi;
        cur_i = cur_r * wi + cur_i * wr;
        cur_r = nr;
      }
    }
  }
}

}  // namespace

PcmAudioSource::PcmAudioSource()
    : ring_(kRingSize, 0.0f), smooth_(kFftSize / 2, 0.0f) {}

void PcmAudioSource::Push(const float* samples, std::size_t n) noexcept {
  std::lock_guard<std::mutex> lk(mu_);
  for (std::size_t i = 0; i < n; ++i)
    ring_[(write_ + i) & (kRingSize - 1)] = samples[i];
  write_ += n;
}

bool PcmAudioSource::Fill(unsigned char* dst) noexcept {
  constexpr int kBins = kFftSize / 2;  // 512
  static_assert(kBins == kAudioTexWidth,
                "FFT bin count must match texture row");

  // Snapshot the latest kFftSize samples under the lock (cheap copy).
  std::vector<float> re(kFftSize), im(kFftSize, 0.0f);
  {
    std::lock_guard<std::mutex> lk(mu_);
    const std::size_t avail = std::min<std::size_t>(write_, kFftSize);
    const std::size_t start = write_ - avail;  // oldest of the window
    for (int i = 0; i < kFftSize; ++i) {
      re[static_cast<std::size_t>(i)] =
          (static_cast<std::size_t>(i) < kFftSize - avail)
              ? 0.0f
              : ring_[(start + static_cast<std::size_t>(i) -
                       (kFftSize - avail)) &
                      (kRingSize - 1)];
    }
  }

  // Waveform row (row 1): map [-1,1] to a byte centred at 128, like Web Audio's
  // getByteTimeDomainData.  Use the most recent kBins samples.
  for (int i = 0; i < kBins; ++i) {
    const float s = re[static_cast<std::size_t>(kFftSize - kBins + i)];
    const float v = 128.0f + 128.0f * s;
    dst[kBins + i] = static_cast<unsigned char>(std::clamp(v, 0.0f, 255.0f));
  }

  // Blackman window before the transform (Web Audio's default), then FFT.
  for (int i = 0; i < kFftSize; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kFftSize - 1);
    const float w = 0.42f - 0.5f * std::cos(2.0f * 3.14159265358979f * t) +
                    0.08f * std::cos(4.0f * 3.14159265358979f * t);
    re[static_cast<std::size_t>(i)] *= w;
  }
  Fft(re, im);

  // FFT row (row 0): per-bin magnitude, temporally smoothed (tau=0.8), then
  // mapped from [minDb,maxDb] = [-100,-30] dB to 0..255, as Web Audio does.
  constexpr float kTau = 0.8f;
  constexpr float kMinDb = -100.0f, kMaxDb = -30.0f;
  const float scale = 255.0f / (kMaxDb - kMinDb);
  for (int i = 0; i < kBins; ++i) {
    const float mag =
        std::sqrt(
            re[static_cast<std::size_t>(i)] * re[static_cast<std::size_t>(i)] +
            im[static_cast<std::size_t>(i)] * im[static_cast<std::size_t>(i)]) /
        static_cast<float>(kFftSize);
    float& sm = smooth_[static_cast<std::size_t>(i)];
    sm = kTau * sm + (1.0f - kTau) * mag;
    const float db = 20.0f * std::log10(sm > 1e-9f ? sm : 1e-9f);
    const float v = scale * (db - kMinDb);
    dst[i] = static_cast<unsigned char>(std::clamp(v, 0.0f, 255.0f));
  }
  return true;
}

std::unique_ptr<AudioSource> MakeMicSource() {
  // Optional override: SHADERTOY_AUDIO_BACKEND = pipewire | alsa | none.
  // Unset/empty selects the first compiled-in back-end (PipeWire, then ALSA).
  const char* pref = std::getenv("SHADERTOY_AUDIO_BACKEND");
  const bool forced = pref != nullptr && pref[0] != '\0';
  if (forced && std::strcmp(pref, "none") == 0)
    return nullptr;
  const auto want = [&](const char* name) {
    return !forced || std::strcmp(pref, name) == 0;
  };
  (void)want;
#if SHADERTOY_HAVE_PIPEWIRE
  if (want("pipewire"))
    return MakePipeWireMicSource();
#endif
#if SHADERTOY_HAVE_ALSA
  if (want("alsa"))
    return MakeAlsaMicSource();
#endif
  return nullptr;  // no capture back-end compiled in (or forced one absent)
}

}  // namespace shadertoy
