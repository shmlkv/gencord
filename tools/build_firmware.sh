#!/usr/bin/env bash
# Build-only. Never invokes upload, a serial port, merged images or device tools.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD/.build-tools"
export HOME="$ROOT/home" PLATFORMIO_CORE_DIR="$ROOT/pio" PLATFORMIO_CACHE_DIR="$ROOT/cache/pio"
export TMPDIR="$ROOT/tmp" PIP_CACHE_DIR="$ROOT/cache/pip" UV_CACHE_DIR="$ROOT/cache/uv"
export XDG_CACHE_HOME="$ROOT/cache" IDF_TOOLS_PATH="$ROOT/home/.platformio"
mkdir -p "$HOME" "$TMPDIR" "$PIP_CACHE_DIR" "$UV_CACHE_DIR" logs
if [ ! -x "$ROOT/venv/bin/pio" ]; then
  uv venv "$ROOT/venv"
fi
uv pip install --python "$ROOT/venv/bin/python" 'platformio==6.2.0' 'click==8.1.8'
"$ROOT/venv/bin/pio" run -e adv-idf54 -j 2 2>&1 | tee logs/build-final.log
