#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# gen_blacklist.sh — generate a per-GPU shader blacklist for shadertoy_egl.
#
# Profiles every shader in a directory offscreen on THIS device's GPU at a small
# proxy resolution (safe — below the GPU's hang watchdog), extrapolates the
# per-frame cost to a target resolution, and writes a blacklist of shaders that:
#   * fail to load/compile         (LOADFAIL)
#   * crash the renderer           (CRASH)
#   * time out even at proxy res   (TIMEOUT)
#   * extrapolate above the budget (HEAVY — would likely hang at target res)
#
# The blacklist is per-GPU; regenerate it on each device.  Feed it to the app:
#   shadertoy_egl --blacklist <blacklist.txt>
#
# Usage:
#   gen_blacklist.sh [options] <shaders_dir>
#
# Options:
#   -o, --out FILE       blacklist output (default: <shaders_dir>/blacklist.txt)
#   -r, --render BIN     path to headless_render (default: auto-locate, else build)
#       --proxy N        proxy square dimension to profile at   (default 192)
#       --target N       target square dimension to extrapolate (default 1920)
#       --budget-ms N    max est. ms/frame@target before HEAVY  (default 300)
#       --timeout N      per-shader timeout in seconds          (default 10)
#       --state DIR      resumable state dir (default: <out>.state)
#   -h, --help           show this help
#
# Resumable: re-run after an interruption to continue; a shader that was mid-
# profile when the run died is recorded CRASH and skipped next time.  Honors
# $SHADERTOY_MEDIA_DIR (passed to the renderer).
set -uo pipefail

die() { echo "gen_blacklist: $*" >&2; exit 1; }
usage() { sed -n '4,33p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

RENDER=""; OUT=""; PROXY=192; TARGET=1920; BUDGET=300; TMO=10; STATE=""; SHDIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out)       OUT="$2"; shift 2;;
    -r|--render)    RENDER="$2"; shift 2;;
    --proxy)        PROXY="$2"; shift 2;;
    --target)       TARGET="$2"; shift 2;;
    --budget-ms)    BUDGET="$2"; shift 2;;
    --timeout)      TMO="$2"; shift 2;;
    --state)        STATE="$2"; shift 2;;
    -h|--help)      usage 0;;
    -*)             die "unknown option: $1 (see --help)";;
    *)              [ -z "$SHDIR" ] && SHDIR="$1" || die "extra argument: $1"; shift;;
  esac
done
[ -n "$SHDIR" ] || usage 1
[ -d "$SHDIR" ] || die "not a directory: $SHDIR"
OUT="${OUT:-$SHDIR/blacklist.txt}"
STATE="${STATE:-$OUT.state}"
repo="$(cd "$(dirname "$0")/.." && pwd)"

# --- locate (or build) headless_render -----------------------------------
if [ -z "$RENDER" ]; then
  for c in "$repo"/build*/examples/headless_render ./build*/examples/headless_render; do
    [ -x "$c" ] && { RENDER="$c"; break; }
  done
  [ -z "$RENDER" ] && RENDER="$(command -v headless_render 2>/dev/null || true)"
fi
if [ -z "$RENDER" ] || [ ! -x "$RENDER" ]; then
  echo "gen_blacklist: headless_render not found — building it..." >&2
  command -v meson >/dev/null && command -v ninja >/dev/null \
    || die "need meson + ninja to build headless_render (or pass --render BIN)"
  ( meson setup "$repo/build" -Dexamples=true >/dev/null 2>&1 \
    || meson setup --reconfigure "$repo/build" -Dexamples=true >/dev/null 2>&1 ) \
    && ninja -C "$repo/build" >/dev/null 2>&1 \
    || die "failed to build headless_render — build it manually and pass --render"
  RENDER="$repo/build/examples/headless_render"
  [ -x "$RENDER" ] || die "build did not produce headless_render (is egl/glesv2 available?)"
fi

mkdir -p "$STATE"
prof="$STATE/profile.tsv"; cur="$STATE/current.txt"; touch "$prof"
scale=$(awk -v t="$TARGET" -v p="$PROXY" 'BEGIN{print (t*t)/(p*p)}')
total=$(find "$SHDIR" -maxdepth 1 -name '*.json' | wc -l)

echo "gen_blacklist: $RENDER" >&2
echo "  device: $(LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-0} "$RENDER" /dev/null /dev/null '' 8 8 1 0 2>&1 | sed -n 's/^GL: //p' | head -1)" >&2
echo "  $total shaders | proxy ${PROXY}^2 -> ${TARGET}^2 | budget ${BUDGET} ms/frame | timeout ${TMO}s" >&2

# resume: a shader mid-profile when the last run died is itself suspect
if [ -s "$cur" ]; then printf '%s\tCRASH\t-\t-\n' "$(<"$cur")" >> "$prof"; : > "$cur"; sync; fi
declare -A done; while IFS=$'\t' read -r id _; do [ -n "$id" ] && done["$id"]=1; done < "$prof"

n=0
while IFS= read -r f; do
  id=$(basename "$f" .json); n=$((n+1))
  [ -n "${done[$id]:-}" ] && continue
  printf '%s' "$id" > "$cur"; sync
  err=$(timeout "$TMO" "$RENDER" "$f" /dev/null "${SHADERTOY_MEDIA_DIR:-}" "$PROXY" "$PROXY" 8 4 2>&1 >/dev/null)
  rc=$?
  if [ "$rc" -eq 0 ]; then
    ms=$(printf '%s\n' "$err" | grep -oE 'render: [0-9.]+' | head -1 | awk '{print $2}')
    est=$(awk -v m="${ms:-0}" -v s="$scale" 'BEGIN{printf "%.0f", m*s}')
    awk -v e="$est" -v b="$BUDGET" 'BEGIN{exit !(e>b)}' && v=HEAVY || v=ok
    printf '%s\t%s\t%s\t%s\n' "$id" "$v" "${ms:-?}" "$est" >> "$prof"
  elif [ "$rc" -eq 1 ]; then printf '%s\tLOADFAIL\t-\t-\n' "$id" >> "$prof"
  elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then printf '%s\tTIMEOUT\t-\t-\n' "$id" >> "$prof"
  else printf '%s\tCRASH%d\t-\t-\n' "$id" "$rc" >> "$prof"; fi
  : > "$cur"; sync
  [ $((n % 200)) -eq 0 ] && echo "  ...$n/$total profiled" >&2
done < <(find "$SHDIR" -maxdepth 1 -name '*.json' | sort)

awk -F'\t' '$2!="ok"{print $1}' "$prof" | sort -u > "$OUT"
echo "gen_blacklist: wrote $OUT ($(wc -l < "$OUT") of $total shaders blacklisted)" >&2
cut -f2 "$prof" | sort | uniq -c | sed 's/^/  /' >&2
