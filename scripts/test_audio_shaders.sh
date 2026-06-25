#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# test_audio_shaders.sh — batch-render the audio shaders to exercise a mic
# back-end, with a media folder so their texture/cubemap channels resolve.
#
# Builds (or reuses) the list of audio shaders, then renders each offscreen via
# headless_render under the chosen audio back-end, passing --media so texture
# channels load instead of warning "cannot load texture ... (using black)".
# Classifies each shader ok / LOADFAIL / TIMEOUT / CRASH and reports how many
# still had unresolved media.
#
# Usage:
#   test_audio_shaders.sh [options] <shaders_dir>
#
# Options:
#   -m, --media DIR    media folder for texture/cubemap channels
#                      (default: $SHADERTOY_MEDIA_DIR)
#   -b, --backend NAME audio back-end: pipewire | alsa | none
#                      (sets $SHADERTOY_AUDIO_BACKEND; default: leave unset)
#       --device DEV   ALSA capture device (sets $SHADERTOY_ALSA_DEVICE)
#   -l, --list FILE    test ids/paths from FILE instead of scanning for audio shaders
#       --ctypes LIST  when scanning, which audio ctypes (default: mic; see
#                      list_audio_shaders.sh)
#   -r, --render BIN   headless_render path (default: auto-locate under build*/)
#   -n, --limit N      test only the first N shaders (0 = all; default: 50)
#       --size N       square render size (default: 256)
#       --frames N     frames per shader (default: 4)
#       --timeout N    per-shader timeout in seconds (default: 15)
#   -h, --help         show this help
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
die() { echo "test_audio_shaders: $*" >&2; exit 1; }
usage() { sed -n '3,33p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

MEDIA="${SHADERTOY_MEDIA_DIR:-}"; BACKEND=""; DEVICE=""; LIST=""; CTYPES="mic"
RENDER=""; LIMIT=50; SIZE=256; FRAMES=4; TMO=15; SHDIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    -m|--media)   MEDIA="$2"; shift 2;;
    -b|--backend) BACKEND="$2"; shift 2;;
    --device)     DEVICE="$2"; shift 2;;
    -l|--list)    LIST="$2"; shift 2;;
    --ctypes)     CTYPES="$2"; shift 2;;
    -r|--render)  RENDER="$2"; shift 2;;
    -n|--limit)   LIMIT="$2"; shift 2;;
    --size)       SIZE="$2"; shift 2;;
    --frames)     FRAMES="$2"; shift 2;;
    --timeout)    TMO="$2"; shift 2;;
    -h|--help)    usage 0;;
    -*)           die "unknown option: $1 (see --help)";;
    *)            [ -z "$SHDIR" ] && SHDIR="$1" || die "extra argument: $1"; shift;;
  esac
done
[ -n "$SHDIR" ] || [ -n "$LIST" ] || usage 1
[ -z "$SHDIR" ] || [ -d "$SHDIR" ] || die "not a directory: $SHDIR"
[ -z "$MEDIA" ] && echo "test_audio_shaders: no --media given; texture channels will be black" >&2
[ -n "$MEDIA" ] && [ ! -d "$MEDIA" ] && die "media dir not found: $MEDIA"

# --- locate headless_render -------------------------------------------------
if [ -z "$RENDER" ]; then
  for c in "$here"/../build*/examples/headless_render; do
    [ -x "$c" ] && { RENDER="$c"; break; }
  done
  [ -z "$RENDER" ] && RENDER="$(command -v headless_render 2>/dev/null || true)"
fi
[ -n "$RENDER" ] && [ -x "$RENDER" ] || die "headless_render not found (build -Dexamples=true, or pass --render)"

# --- build the work list (paths) --------------------------------------------
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
paths="$tmp/paths.txt"
if [ -n "$LIST" ]; then
  [ -f "$LIST" ] || die "list file not found: $LIST"
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    if [ -f "$line" ]; then echo "$line"
    elif [ -n "$SHDIR" ] && [ -f "$SHDIR/$line.json" ]; then echo "$SHDIR/$line.json"
    fi
  done < "$LIST" > "$paths"
else
  "$here/list_audio_shaders.sh" --ctypes "$CTYPES" --paths --no-tsv \
      -o "$paths" "$SHDIR" >/dev/null 2>"$tmp/scan.err" \
    || { cat "$tmp/scan.err" >&2; die "scan failed"; }
fi
[ "$LIMIT" -gt 0 ] && { head -n "$LIMIT" "$paths" > "$paths.cut"; mv "$paths.cut" "$paths"; }
n_total=$(wc -l < "$paths")
[ "$n_total" -gt 0 ] || die "no shaders to test"

echo "test_audio_shaders: $RENDER" >&2
echo "  shaders: $n_total | backend: ${BACKEND:-default} | media: ${MEDIA:-none} | ${SIZE}^2 x ${FRAMES}f" >&2
[ -n "$BACKEND" ] && export SHADERTOY_AUDIO_BACKEND="$BACKEND"
[ -n "$DEVICE" ]  && export SHADERTOY_ALSA_DEVICE="$DEVICE"
export SHADERTOY_MEDIA_DIR="$MEDIA"

ok=0; loadfail=0; timeout_n=0; crash=0; media_miss=0
while IFS= read -r f; do
  id=$(basename "$f" .json)
  err=$(LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1} GALLIUM_DRIVER=${GALLIUM_DRIVER:-llvmpipe} \
        timeout "$TMO" "$RENDER" "$f" /dev/null "$MEDIA" "$SIZE" "$SIZE" "$FRAMES" 2 2>&1 >/dev/null)
  rc=$?
  miss=$(printf '%s\n' "$err" | grep -c 'cannot load texture')
  [ "$miss" -gt 0 ] && media_miss=$((media_miss + 1))
  case "$rc" in
    0)        ok=$((ok+1)); tag="ok";;
    1)        loadfail=$((loadfail+1)); tag="LOADFAIL";;
    124|137)  timeout_n=$((timeout_n+1)); tag="TIMEOUT";;
    *)        crash=$((crash+1)); tag="CRASH$rc";;
  esac
  printf '  %-10s %s%s\n' "$tag" "$id" \
    "$([ "$miss" -gt 0 ] && echo "  [${miss} media missing]")" >&2
done < "$paths"

echo "test_audio_shaders: $ok ok, $loadfail loadfail, $timeout_n timeout, $crash crash" \
     "($media_miss of $n_total had unresolved media)" >&2
[ "$loadfail" -eq 0 ] && [ "$crash" -eq 0 ]  # nonzero exit if anything failed to render
