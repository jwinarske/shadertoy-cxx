// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// audio_pipewire.hpp — opt-in PipeWire entry point for sharing one process
// connection across many streams.
//
// The generic <shadertoy/audio.hpp> MakeMicSource() opens a self-contained
// PipeWire connection (its own context, core, and thread loop).  An app that
// already runs a PipeWire thread loop — typically because it also drives video
// (camera/screen) streams — should instead create the mic source on its own
// loop and core so every audio and video stream rides a single daemon
// connection and a single thread.  This header is the only place PipeWire types
// enter the public API, and is installed only when the PipeWire backend is
// compiled in (SHADERTOY_HAVE_PIPEWIRE).

#pragma once

#include <shadertoy/audio.hpp>
#include <shadertoy/config.hpp>

#include <memory>

#if SHADERTOY_HAVE_PIPEWIRE

// Opaque PipeWire handles — forward-declared so consumers of this header are
// not forced to include <pipewire/pipewire.h> unless they use these types.
struct pw_thread_loop;
struct pw_core;

namespace shadertoy {

/// Create a microphone source on an app-owned PipeWire connection.  The source
/// creates a pw_stream on @p core and takes the @p loop lock around stream
/// operations, instead of opening its own connection.  The app owns and must
/// outlive @p loop and @p core, and must have started the loop
/// (pw_thread_loop_start) before AudioSource::Start() is called. Stop()/destroy
/// the source before destroying the loop or core.  Returns nullptr if @p loop
/// or
/// @p core is null.
[[nodiscard]] std::unique_ptr<AudioSource> MakePipeWireMicSource(
    pw_thread_loop* loop,
    pw_core* core);

}  // namespace shadertoy

#endif  // SHADERTOY_HAVE_PIPEWIRE
