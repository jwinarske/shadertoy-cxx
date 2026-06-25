#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# persist_sweep.sh — crash-resilient, resumable shader test runner.
#
#   persist_sweep.sh <shadertoy_egl> <shaders_dir> <state_dir> [dwell_s] [idlist]
#
# Runs each shader (one at a time) and records the outcome to disk so the run
# survives a session/compositor/GPU crash.  State (all in <state_dir>):
#   results.tsv   "<id>\t<status>"  appended AFTER each shader finishes
#   current.txt   "<id>"            written BEFORE a shader runs, cleared after
#   blacklist.txt "<id>"            ids to skip (one per line)
#
# Crash recovery: on launch a non-empty current.txt names the shader that was
# running when the last session died — it is recorded CRASHED-SESSION and added
# to blacklist.txt.  Done and blacklisted ids are skipped, so re-launching the
# identical command after a restart resumes where it left off and never retries
# the shader that crashed the session.
#
# $SHADERTOY_MEDIA_DIR, if set, is passed to the app as --media.
set -uo pipefail
BIN="${1:?usage: $0 <app> <shaders_dir> <state_dir> [dwell_s] [idlist]}"
SHDIR="${2:?shaders_dir required}"
OUT="${3:?state_dir required}"
DWELL="${4:-10}"
LIST="${5:-}"
mkdir -p "$OUT"
results="$OUT/results.tsv"; current="$OUT/current.txt"; black="$OUT/blacklist.txt"
touch "$results" "$black"

media_args=()
[ -n "${SHADERTOY_MEDIA_DIR:-}" ] && media_args=(--media "$SHADERTOY_MEDIA_DIR")

# Recover from a prior crash.
if [ -s "$current" ]; then
  cid=$(<"$current")
  printf '%s\tCRASHED-SESSION\n' "$cid" >> "$results"
  grep -qxF "$cid" "$black" 2>/dev/null || echo "$cid" >> "$black"
  : > "$current"; sync
  echo "recovered: '$cid' crashed the last session -> blacklisted" >&2
fi

# Work list.
ids=()
if [ -n "$LIST" ]; then
  mapfile -t ids < "$LIST"
else
  while IFS= read -r f; do ids+=("$(basename "$f" .json)"); done \
    < <(find "$SHDIR" -maxdepth 1 -name '*.json' | sort)
fi

# Skip sets (done + blacklisted).
declare -A skip
while IFS=$'\t' read -r id _; do [ -n "$id" ] && skip["$id"]=1; done < "$results"
while IFS= read -r id;        do [ -n "$id" ] && skip["$id"]=1; done < "$black"

tested=0
for id in "${ids[@]}"; do
  [ -n "${skip[$id]:-}" ] && continue
  [ -f "$SHDIR/$id.json" ] || { printf '%s\tMISSING\n' "$id" >> "$results"; continue; }
  printf '%s' "$id" > "$current"; sync                 # mark BEFORE running
  timeout "$DWELL" "$BIN" "${media_args[@]}" "$SHDIR/$id.json" >/dev/null 2>&1
  rc=$?
  case $rc in 124) st=ok;; 0) st=exit0;; 139) st=segv;; 134) st=abort;;
              1) st=loadfail;; *) st="exit$rc";; esac
  printf '%s\t%s\n' "$id" "$st" >> "$results"          # record AFTER
  : > "$current"; sync                                 # clear marker
  tested=$((tested+1))
done
echo "complete: tested $tested this run; $(wc -l < "$results") total; $(sort -u "$black" | wc -l) blacklisted"
