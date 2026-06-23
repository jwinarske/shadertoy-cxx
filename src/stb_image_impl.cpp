// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// stb_image_impl.cpp — single translation unit that instantiates stb_image.
// (stb_image.h itself is public domain; see third_party/stb/stb_image.h.)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DUMMY  // keep stdio loader (stbi_load) available

// stb is third-party; quiet its warnings so it builds clean under -Wall/-Wextra.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include "stb_image.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
