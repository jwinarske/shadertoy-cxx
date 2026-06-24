#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# shadertoy_assets.sh — map a Shadertoy export's texture/cubemap inputs to the
# local filenames the GL ES renderer resolves (basename; cubemaps expand to the
# six faces <stem>.ext, <stem>_1.ext .. <stem>_5.ext).
#
#   shadertoy_assets.sh <shader.json> [media_dir]
#     no media_dir : print a manifest  -> "<ctype>\t<URL>\t<local_filename>"
#     media_dir    : download each asset into media_dir (needs a logged-in
#                    session; pass cookies via CURL_OPTS if Shadertoy 403s)
#
# Then run:  shadertoy_egl --media <media_dir> <shader.json>
# Downloaded media remains under its authors' licenses — respect those terms.
set -euo pipefail
json="${1:?usage: $0 <shader.json> [media_dir]}"
dir="${2:-}"
base="https://www.shadertoy.com"

# Emit "<ctype>\t<local_filename>" per asset (or "skip\t<src>" for presets).
python3 - "$json" <<'PY' |
import json, os, sys
s = json.load(open(sys.argv[1]))
s = s.get("Shader", s)
seen = set()
for p in s.get("renderpass", []):
    for i in p.get("inputs", []):
        c, src = i.get("ctype", ""), i.get("src", "")
        if c not in ("texture", "cubemap") or src in seen:
            continue
        seen.add(src)
        # Real user assets are exactly "/media/a/<file>"; presets carry an extra
        # path component ("/media/a//media/previz/...", "/presets/...").
        if (not src.startswith("/media/a/") or "/previz/" in src
                or "/presets/" in src or src.count("/media/") > 1):
            print("skip", src, sep="\t"); continue
        stem, ext = os.path.splitext(os.path.basename(src))
        names = [stem + ext] if c == "texture" else \
                [stem + ext] + [f"{stem}_{n}{ext}" for n in range(1, 6)]
        for n in names:
            print(c, n, sep="\t")
PY
while IFS=$'\t' read -r ctype name; do
  if [ "$ctype" = skip ]; then
    echo "# skip (preset / non-/media/a): $name" >&2
    continue
  fi
  url="$base/media/a/$name"
  if [ -z "$dir" ]; then
    printf '%s\t%s\t%s\n' "$ctype" "$url" "$name"
  else
    mkdir -p "$dir"
    if curl -fsSL ${CURL_OPTS:-} "$url" -o "$dir/$name"; then
      echo "ok   $name"
    else
      echo "FAIL $name ($url)" >&2
    fi
  fi
done
