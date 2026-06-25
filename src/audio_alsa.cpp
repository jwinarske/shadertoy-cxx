// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// audio_alsa.cpp — ALSA microphone capture back-end.
//
// Compiled only when libasound is available (SHADERTOY_HAVE_ALSA).  Opens an
// ALSA capture PCM (the "default" device, or $SHADERTOY_ALSA_DEVICE) and reads
// interleaved S16 frames on a dedicated thread, down-mixing to mono and feeding
// PcmAudioSource's ring; the render thread reads the texture through Fill().
//
// The "default" device routes through ALSA's plug layer, which converts rate /
// format / channel count for us, so this works against any capture card
// (e.g. a USB mic) without the hardware natively supporting S16/mono/44100.

#include "audio_internal.hpp"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

namespace shadertoy {
namespace {

class AlsaMicSource final : public PcmAudioSource {
 public:
  ~AlsaMicSource() override { Stop(); }

  bool Start() override;
  void Stop() noexcept override;

 private:
  void Run();  // capture-thread body

  snd_pcm_t* pcm_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  unsigned channels_ = 1;
  snd_pcm_uframes_t period_ = 0;
};

bool AlsaMicSource::Start() {
  if (pcm_ != nullptr)
    return true;  // already running

  const char* dev = std::getenv("SHADERTOY_ALSA_DEVICE");
  if (dev == nullptr || dev[0] == '\0')
    dev = "default";

  if (snd_pcm_open(&pcm_, dev, SND_PCM_STREAM_CAPTURE, 0) < 0) {
    pcm_ = nullptr;
    return false;
  }

  unsigned rate = 44100;
  channels_ = 1;
  snd_pcm_hw_params_t* hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);
  bool ok =
      snd_pcm_hw_params_any(pcm_, hw) >= 0 &&
      snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED) >=
          0 &&
      snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE) >= 0 &&
      snd_pcm_hw_params_set_channels(pcm_, hw, channels_) >= 0 &&
      snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr) >= 0 &&
      snd_pcm_hw_params(pcm_, hw) >= 0;
  if (ok) {
    snd_pcm_hw_params_get_channels(hw, &channels_);
    snd_pcm_hw_params_get_period_size(hw, &period_, nullptr);
  }
  if (!ok || period_ == 0) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    return false;
  }

  running_.store(true);
  thread_ = std::thread(&AlsaMicSource::Run, this);
  return true;
}

void AlsaMicSource::Run() {
  const unsigned ch = channels_ ? channels_ : 1;
  std::vector<int16_t> buf(static_cast<size_t>(period_) * ch);
  std::vector<float> mono(period_);
  while (running_.load()) {
    const snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buf.data(), period_);
    if (n < 0) {
      // Recover from xruns/suspends and keep going.
      if (snd_pcm_recover(pcm_, static_cast<int>(n), 1) < 0)
        break;
      continue;
    }
    const size_t frames = static_cast<size_t>(n);
    for (size_t f = 0; f < frames; ++f) {
      int32_t acc = 0;
      for (unsigned c = 0; c < ch; ++c)
        acc += buf[f * ch + c];
      mono[f] = static_cast<float>(acc) / (static_cast<float>(ch) * 32768.0f);
    }
    Push(mono.data(), frames);
  }
}

void AlsaMicSource::Stop() noexcept {
  running_.store(false);
  if (thread_.joinable())
    thread_.join();
  if (pcm_ != nullptr) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
}

}  // namespace

std::unique_ptr<AudioSource> MakeAlsaMicSource() {
  return std::make_unique<AlsaMicSource>();
}

}  // namespace shadertoy
