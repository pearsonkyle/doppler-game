#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if [[ "$(uname)" == "Darwin" ]]; then
  clang -Os dilation.c -o dilation \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    -lSDL2 -framework OpenGL -lm
else
  cc -Os dilation.c -o dilation \
    $(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2") -lGL -lm
fi

echo "[build] dilation ready ($(ls -lh dilation | awk '{print $5}'))"
