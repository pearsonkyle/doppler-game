#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

clang -Os doppler_updated.c -o doppler \
  -I/opt/homebrew/include \
  -L/opt/homebrew/lib \
  -lSDL2 -framework OpenGL -lm

echo "[build] doppler ready ($(ls -lh doppler | awk '{print $5}'))"
