#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch a Shadertoy shader's JSON into a local directory for personal use.
#
#   fetch_shadertoy.sh <shaderID> [out_dir]
#
# Requires a Shadertoy API key in $SHADERTOY_API_KEY (get one at
# https://www.shadertoy.com/howto#q2). Downloaded shaders remain under their
# authors' licenses — respect those terms; do not redistribute.
set -euo pipefail

id="${1:-}"
out="${2:-.}"
if [[ -z "$id" ]]; then
  echo "usage: $0 <shaderID> [out_dir]" >&2
  exit 2
fi
if [[ -z "${SHADERTOY_API_KEY:-}" ]]; then
  echo "error: set SHADERTOY_API_KEY (see https://www.shadertoy.com/howto#q2)" >&2
  exit 1
fi
mkdir -p "$out"
url="https://www.shadertoy.com/api/v1/shaders/${id}?key=${SHADERTOY_API_KEY}"
curl -fsSL "$url" -o "$out/${id}.json"
if grep -q '"Error"' "$out/${id}.json"; then
  echo "error: Shadertoy API returned an error for $id" >&2
  cat "$out/${id}.json" >&2
  rm -f "$out/${id}.json"
  exit 1
fi
echo "saved $out/${id}.json"
