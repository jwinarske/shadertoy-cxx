// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// audio_pipewire.cpp — PipeWire microphone capture back-end.
//
// Compiled only when libpipewire-0.3 is available (SHADERTOY_HAVE_PIPEWIRE).
// Captures mono float PCM from the default audio source on a private PipeWire
// thread loop and feeds it into PcmAudioSource's ring; the render thread reads
// the analyzed texture through Fill().

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
  ~PipeWireMicSource() override { Stop(); }

  bool Start() override;
  void Stop() noexcept override;

  // PipeWire C callbacks (public so the file-scope event table can bind them).
  static void OnParamChanged(void* data, uint32_t id, const spa_pod* param);
  static void OnProcess(void* data);

 private:
  pw_thread_loop* loop_ = nullptr;
  pw_stream* stream_ = nullptr;
  uint32_t channels_ = 1;  // negotiated channel count (set on the PW thread)
  bool pw_inited_ = false;
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

bool PipeWireMicSource::Start() {
  if (stream_ != nullptr)
    return true;  // already running

  pw_init(nullptr, nullptr);
  pw_inited_ = true;

  loop_ = pw_thread_loop_new("shadertoy-mic", nullptr);
  if (loop_ == nullptr) {
    Stop();
    return false;
  }

  pw_properties* props =
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                        "Capture", PW_KEY_MEDIA_ROLE, "Music", nullptr);
  stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_),
                                 "shadertoy-mic", props, &StreamEvents(), this);
  if (stream_ == nullptr) {
    Stop();
    return false;
  }

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
  const int rc = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                                   flags, params, 1);
  if (rc < 0) {
    Stop();
    return false;
  }

  if (pw_thread_loop_start(loop_) < 0) {
    Stop();
    return false;
  }
  return true;
}

void PipeWireMicSource::Stop() noexcept {
  if (loop_ != nullptr)
    pw_thread_loop_stop(loop_);
  if (stream_ != nullptr) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
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

}  // namespace shadertoy
