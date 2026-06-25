// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// audio_pipewire.cpp — PipeWire microphone capture backend.
//
// Compiled only when libpipewire-0.3 is available (SHADERTOY_HAVE_PIPEWIRE).
// Captures mono float PCM from the default audio source and feeds it into
// PcmAudioSource's ring; the render thread reads the analyzed texture through
// Fill().
//
// Two ownership modes share one stream-setup path:
//   * owned  — the source creates its own pw_thread_loop + pw_context + pw_core
//              (MakePipeWireMicSource()); correct for a standalone single mic.
//   * shared — the source attaches to an app-owned loop + core and creates only
//              a pw_stream on it (MakePipeWireMicSource(loop, core), declared
//              in <shadertoy/audio_pipewire.hpp>), so many audio (and video)
//              streams ride one daemon connection and one thread.

#include "shadertoy/audio_pipewire.hpp"
#include "audio_internal.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <cstdint>
#include <vector>

namespace shadertoy {
namespace {

class PipeWireMicSource final : public PcmAudioSource {
 public:
  PipeWireMicSource() = default;                          // owned mode
  PipeWireMicSource(pw_thread_loop* loop, pw_core* core)  // shared mode
      : ext_loop_(loop), ext_core_(core) {}
  ~PipeWireMicSource() override { Stop(); }

  bool Start() override;
  void Stop() noexcept override;

  // PipeWire C callbacks (public so the file-scope event table can bind them).
  static void OnParamChanged(void* data, uint32_t id, const spa_pod* param);
  static void OnProcess(void* data);

 private:
  bool
  CreateStream();  // stream new + listener + connect (caller holds the lock)

  // Active handles (point at owned objects, or at the injected loop/core).
  pw_thread_loop* loop_ = nullptr;
  pw_core* core_ = nullptr;
  pw_stream* stream_ = nullptr;
  spa_hook listener_{};

  // Owned-mode objects (null in shared mode).
  pw_context* context_ = nullptr;
  bool pw_inited_ = false;

  // Injected (not owned); non-null selects shared mode.
  pw_thread_loop* ext_loop_ = nullptr;
  pw_core* ext_core_ = nullptr;

  uint32_t channels_ = 1;  // negotiated channel count (set on the PW thread)
};

const pw_stream_events& StreamEvents() {
  // Built field-by-field (not via designated initializers) to stay C++17-clean.
  static const pw_stream_events ev = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.param_changed = &PipeWireMicSource::OnParamChanged;
    e.process = &PipeWireMicSource::OnProcess;
    return e;
  }();
  return ev;
}

void PipeWireMicSource::OnParamChanged(void* data,
                                       uint32_t id,
                                       const spa_pod* param) {
  auto* self = static_cast<PipeWireMicSource*>(data);
  if (param == nullptr || id != SPA_PARAM_Format)
    return;
  uint32_t media_type = 0, media_subtype = 0;
  if (spa_format_parse(param, &media_type, &media_subtype) < 0)
    return;
  if (media_type != SPA_MEDIA_TYPE_audio ||
      media_subtype != SPA_MEDIA_SUBTYPE_raw)
    return;
  spa_audio_info_raw info{};
  if (spa_format_audio_raw_parse(param, &info) < 0)
    return;
  self->channels_ = info.channels ? info.channels : 1;
}

void PipeWireMicSource::OnProcess(void* data) {
  auto* self = static_cast<PipeWireMicSource*>(data);
  pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
  if (b == nullptr)
    return;
  spa_buffer* buf = b->buffer;
  const auto* src = static_cast<const float*>(buf->datas[0].data);
  if (src != nullptr && buf->datas[0].chunk->size > 0) {
    const uint32_t floats = buf->datas[0].chunk->size / sizeof(float);
    const uint32_t ch = self->channels_ ? self->channels_ : 1;
    const uint32_t frames = floats / ch;
    if (ch == 1) {
      self->Push(src, frames);
    } else {
      // Down-mix interleaved frames to mono by averaging channels.
      std::vector<float> mono(frames);
      for (uint32_t f = 0; f < frames; ++f) {
        float acc = 0.0f;
        for (uint32_t c = 0; c < ch; ++c)
          acc += src[f * ch + c];
        mono[f] = acc / static_cast<float>(ch);
      }
      self->Push(mono.data(), frames);
    }
  }
  pw_stream_queue_buffer(self->stream_, b);
}

bool PipeWireMicSource::CreateStream() {
  pw_properties* props =
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                        "Capture", PW_KEY_MEDIA_ROLE, "Music", nullptr);
  stream_ = pw_stream_new(core_, "shadertoy-mic", props);  // consumes props
  if (stream_ == nullptr)
    return false;
  pw_stream_add_listener(stream_, &listener_, &StreamEvents(), this);

  uint8_t pod_buf[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
  spa_audio_info_raw req{};
  req.format = SPA_AUDIO_FORMAT_F32;
  req.channels = 1;
  req.rate = 44100;
  const spa_pod* params[1] = {
      spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &req)};

  // The OR of two pw_stream_flags has no named enumerator; the cast back to the
  // flag enum is the documented PipeWire idiom.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS);
  return pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags,
                           params, 1) >= 0;
}

bool PipeWireMicSource::Start() {
  if (stream_ != nullptr)
    return true;  // already running

  if (ext_loop_ != nullptr) {
    // Shared: ride the app-owned loop + core (the app started the loop).
    loop_ = ext_loop_;
    core_ = ext_core_;
  } else {
    // Owned: stand up a private connection and start its thread loop.
    pw_init(nullptr, nullptr);
    pw_inited_ = true;
    loop_ = pw_thread_loop_new("shadertoy-mic", nullptr);
    if (loop_ == nullptr) {
      Stop();
      return false;
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr || pw_thread_loop_start(loop_) < 0) {
      Stop();
      return false;
    }
  }

  // Stream creation (and, when owned, the core connect) must run under the loop
  // lock since the loop is now running.
  pw_thread_loop_lock(loop_);
  if (core_ == nullptr)
    core_ = pw_context_connect(context_, nullptr, 0);
  const bool ok = core_ != nullptr && CreateStream();
  pw_thread_loop_unlock(loop_);
  if (!ok) {
    Stop();
    return false;
  }
  return true;
}

void PipeWireMicSource::Stop() noexcept {
  if (ext_loop_ != nullptr) {
    // Shared: tear down only our stream; the app owns loop/core.
    if (stream_ != nullptr) {
      pw_thread_loop_lock(loop_);
      pw_stream_destroy(stream_);
      pw_thread_loop_unlock(loop_);
      stream_ = nullptr;
    }
    loop_ = nullptr;
    core_ = nullptr;
    return;
  }

  // Owned: stop the loop, then tear the connection down (no lock once stopped).
  if (loop_ != nullptr)
    pw_thread_loop_stop(loop_);
  if (stream_ != nullptr) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
  if (core_ != nullptr) {
    pw_core_disconnect(core_);
    core_ = nullptr;
  }
  if (context_ != nullptr) {
    pw_context_destroy(context_);
    context_ = nullptr;
  }
  if (loop_ != nullptr) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
  if (pw_inited_) {
    pw_deinit();
    pw_inited_ = false;
  }
}

}  // namespace

std::unique_ptr<AudioSource> MakePipeWireMicSource() {
  return std::make_unique<PipeWireMicSource>();
}

std::unique_ptr<AudioSource> MakePipeWireMicSource(pw_thread_loop* loop,
                                                   pw_core* core) {
  if (loop == nullptr || core == nullptr)
    return nullptr;
  return std::make_unique<PipeWireMicSource>(loop, core);
}

}  // namespace shadertoy
