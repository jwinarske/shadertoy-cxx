#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# gen_blacklist.sh — pre-flight GPU compatibility filter.
#
#   gen_blacklist.sh <headless_render> <shaders_dir> <out_dir> \
#                    [proxy_dim] [target_dim] [budget_ms] [timeout_s]
#
# Profiles every shader offscreen on the current EGL device (the GPU unless
# LIBGL_ALWAYS_SOFTWARE=1) at a small proxy resolution — far below the GPU's
# hang watchdog, so profiling itself is safe — then extrapolates the per-frame
# cost to the target resolution.  A shader is blacklisted if it:
#   * fails to load/compile        (LOADFAIL)
#   * crashes the renderer         (CRASH<rc>)
#   * times out even at proxy res  (TIMEOUT)
#   * extrapolates above budget_ms (HEAVY — would likely hang at target res)
#
# Output: <out_dir>/blacklist.txt (one shader id per line) for the app's
# --blacklist, plus profile.tsv (id, verdict, proxy_ms/frame, est_ms@target).
# Resumable: re-run after an interruption and it continues; a shader that was
# mid-profile when the session died is recorded CRASH and skipped next time.
#
# Defaults: proxy 192, target 1920, budget 300 ms/frame@target, timeout 10 s.
set -uo pipefail
RENDER="${1:?usage: $0 <headless_render> <shaders_dir> <out_dir> [proxy target budget_ms timeout_s]}"
SHDIR="${2:?shaders_dir required}"
OUT="${3:?out_dir required}"
PROXY="${4:-192}"; TARGET="${5:-1920}"; BUDGET="${6:-300}"; TMO="${7:-10}"
MEDIA="${SHADERTOY_MEDIA_DIR:-}"
mkdir -p "$OUT"
prof="$OUT/profile.tsv"; cur="$OUT/current.txt"; bl="$OUT/blacklist.txt"
touch "$prof"
scale=$(awk -v t="$TARGET" -v p="$PROXY" 'BEGIN{print (t*t)/(p*p)}')  # pixel ratio

# Recover: a shader mid-profile when the last run died is itself suspect.
if [ -s "$cur" ]; then
  printf '%s\tCRASH\t-\t-\n' "$(<"$cur")" >> "$prof"; : > "$cur"; sync
fi
declare -A done
while IFS=$'\t' read -r id _; do [ -n "$id" ] && done["$id"]=1; done < "$prof"

while IFS= read -r f; do
  id=$(basename "$f" .json)
  [ -n "${done[$id]:-}" ] && continue
  printf '%s' "$id" > "$cur"; sync
  err=$(timeout "$TMO" "$RENDER" "$f" /dev/null "$MEDIA" "$PROXY" "$PROXY" 8 4 2>&1 >/dev/null)
  rc=$?
  if [ "$rc" -eq 0 ]; then
    ms=$(printf '%s\n' "$err" | grep -oE 'render: [0-9.]+' | head -1 | awk '{print $2}')
    est=$(awk -v m="${ms:-0}" -v s="$scale" 'BEGIN{printf "%.0f", m*s}')
    if awk -v e="$est" -v b="$BUDGET" 'BEGIN{exit !(e>b)}'; then v=HEAVY; else v=ok; fi
    printf '%s\t%s\t%s\t%s\n' "$id" "$v" "${ms:-?}" "$est" >> "$prof"
  elif [ "$rc" -eq 1 ]; then
    printf '%s\tLOADFAIL\t-\t-\n' "$id" >> "$prof"
  elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    printf '%s\tTIMEOUT\t-\t-\n' "$id" >> "$prof"
  else
    printf '%s\tCRASH%d\t-\t-\n' "$id" "$rc" >> "$prof"
  fi
  : > "$cur"; sync
done < <(find "$SHDIR" -maxdepth 1 -name '*.json' | sort)

awk -F'\t' '$2!="ok"{print $1}' "$prof" | sort -u > "$bl"
echo "profiled $(wc -l < "$prof") shaders; blacklisted $(wc -l < "$bl")" \
     "(proxy ${PROXY}^2 -> ${TARGET}^2, budget ${BUDGET} ms/frame)"
echo "verdicts:"; cut -f2 "$prof" | sort | uniq -c | sed 's/^/  /'
