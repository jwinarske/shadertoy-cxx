#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# list_audio_shaders.sh — list Shadertoy shaders that bind an audio input.
#
# Scans a directory of Shadertoy JSON files and emits the ones with at least one
# channel whose ctype is in the audio family (music, musicstream, mic,
# soundcloud) — the ctypes the loader maps to ChannelKind::kAudio, i.e. the
# shaders worth testing the PipeWire / ALSA mic back-ends against.
#
# Two stages, like gen_blacklist's pre-filter: a fast grep narrows the 30k+
# files to candidates, then jq authoritatively parses each candidate's
# renderpass inputs (so a stray "music" in code or a description never counts).
#
# Output: the shader ids (one per line) to --out, plus a sidecar <out>.tsv with
# one row per audio channel: id <TAB> pass <TAB> channel <TAB> ctype <TAB> src.
#
# Usage:
#   list_audio_shaders.sh [options] <shaders_dir>
#
# Options:
#   -o, --out FILE     id-list output    (default: <shaders_dir>/audio_shaders.txt)
#       --ctypes LIST  comma-separated ctypes to match
#                                        (default: music,musicstream,mic,soundcloud)
#       --split        also write a separate per-ctype list beside --out, e.g.
#                      audio_shaders.music.txt / .musicstream.txt / .mic.txt
#       --paths        emit file paths instead of bare ids (handy to pipe to a loop)
#       --no-tsv       skip the per-channel <out>.tsv detail sidecar
#   -h, --help         show this help
#
# Honors nothing in the environment; safe to re-run (overwrites the outputs).
set -uo pipefail

die()   { echo "list_audio_shaders: $*" >&2; exit 1; }
usage() { sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

OUT=""; CTYPES="music,musicstream,mic,soundcloud"; PATHS=0; TSV=1; SPLIT=0; SHDIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out)    OUT="$2"; shift 2;;
    --ctypes)    CTYPES="$2"; shift 2;;
    --split)     SPLIT=1; shift;;
    --paths)     PATHS=1; shift;;
    --no-tsv)    TSV=0; shift;;
    -h|--help)   usage 0;;
    -*)          die "unknown option: $1 (see --help)";;
    *)           [ -z "$SHDIR" ] && SHDIR="$1" || die "extra argument: $1"; shift;;
  esac
done
[ -n "$SHDIR" ] || usage 1
[ -d "$SHDIR" ] || die "not a directory: $SHDIR"
command -v jq >/dev/null || die "jq is required"
OUT="${OUT:-$SHDIR/audio_shaders.txt}"
tsv_out="$OUT.tsv"

# Build the matched-ctype set as a regex alternation (grep) and a JSON array (jq).
alt="${CTYPES//,/|}"
jq_set="$(printf '%s' "$CTYPES" | jq -R 'split(",")')"

total=$(find "$SHDIR" -maxdepth 1 -name '*.json' | wc -l)
echo "list_audio_shaders: scanning $total shaders in $SHDIR" >&2
echo "  ctypes: $CTYPES" >&2

# Stage 1 — fast pre-filter: files that mention a matched ctype at all.
# find|xargs (not a *.json glob) so it scales past ARG_MAX on huge mirrors.
mapfile -t cand < <(
  find "$SHDIR" -maxdepth 1 -name '*.json' -print0 \
    | xargs -0 grep -lE "\"ctype\"[[:space:]]*:[[:space:]]*\"($alt)\"" 2>/dev/null \
    | sort)
echo "  candidates after grep pre-filter: ${#cand[@]}" >&2
[ "${#cand[@]}" -eq 0 ] && { : > "$OUT"; [ "$TSV" -eq 1 ] && : > "$tsv_out"; \
  echo "list_audio_shaders: no audio shaders found" >&2; exit 0; }

# Stage 2 — authoritative parse: emit "<id>\t<pass>\t<channel>\t<ctype>\t<src>".
# id is the filename stem (what headless_render / the blacklist key on).
parse_one() {
  local f="$1" id; id=$(basename "$f" .json)
  jq -r --arg id "$id" --argjson set "$jq_set" '
    (.Shader // empty) as $s
    | $s.renderpass[]? as $p
    | $p.inputs[]?
    | select((.ctype // "") as $c | $set | index($c))
    | [$id, ($p.name // $p.type // "?"),
       (.channel // -1 | tostring), (.ctype // "?"),
       (if (.src // "") != "" then "media" else "-" end)] | @tsv
  ' "$f" 2>/dev/null
}

# Map a stream of shader ids (stdin) to ids or full paths, per --paths.
emit() { if [ "$PATHS" -eq 1 ]; then sed "s#^#$SHDIR/#; s#\$#.json#"; else cat; fi; }

: > "$tsv_out"
for f in "${cand[@]}"; do parse_one "$f"; done >> "$tsv_out"

# Combined id list (unique, sorted) from the detail rows.
cut -f1 "$tsv_out" | sort -u | emit > "$OUT"
matched=$(wc -l < "$OUT")
echo "list_audio_shaders: wrote $OUT ($matched shaders with an audio channel)" >&2

# Per-ctype lists beside --out: <base>.<ctype>.<ext> (a shader appears in each
# list whose ctype it binds).
if [ "$SPLIT" -eq 1 ]; then
  base="${OUT%.*}"; ext="${OUT##*.}"; [ "$base" = "$OUT" ] && { base="$OUT"; ext="txt"; }
  IFS=',' read -ra _cts <<< "$CTYPES"
  for c in "${_cts[@]}"; do
    cf="$base.$c.$ext"
    awk -F'\t' -v c="$c" '$4==c{print $1}' "$tsv_out" | sort -u | emit > "$cf"
    n=$(wc -l < "$cf")
    if [ "$n" -gt 0 ]; then echo "  split: $cf ($n)" >&2; else rm -f "$cf"; fi
  done
fi

if [ "$TSV" -eq 1 ]; then
  echo "  detail: $tsv_out ($(wc -l < "$tsv_out") audio channels)" >&2
  echo "  by ctype:" >&2
  cut -f4 "$tsv_out" | sort | uniq -c | sort -rn | sed 's/^/    /' >&2
else
  rm -f "$tsv_out"
fi
