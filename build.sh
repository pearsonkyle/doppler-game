#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if [[ "$(uname)" == "Darwin" ]]; then
  clang -Os doppler.c -o doppler \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    -lSDL2 -framework OpenGL -lm
else
  cc -Os doppler.c -o doppler \
    $(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2") -lGL -lm
fi

echo "[build] doppler ready ($(ls -lh doppler | awk '{print $5}'))"
