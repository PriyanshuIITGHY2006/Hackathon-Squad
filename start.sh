#!/usr/bin/env bash
# Build the engine and start the MWIS workbench.
#   ./start.sh              → http://127.0.0.1:8000
#   ./start.sh --port 9000  → any other port
set -euo pipefail
cd "$(dirname "$0")"

missing=()
command -v g++     >/dev/null 2>&1 || missing+=("gcc")
command -v make    >/dev/null 2>&1 || missing+=("make")
command -v python3 >/dev/null 2>&1 || missing+=("python")

if [ ${#missing[@]} -gt 0 ]; then
  echo "Missing: ${missing[*]}"
  echo
  if command -v pacman >/dev/null 2>&1; then
    echo "  sudo pacman -S --needed base-devel python"
  elif command -v apt >/dev/null 2>&1; then
    echo "  sudo apt install build-essential python3"
  elif command -v dnf >/dev/null 2>&1; then
    echo "  sudo dnf install gcc-c++ make python3"
  fi
  exit 1
fi

echo "Building the engine…"
make -C engine

echo
python3 server/server.py "$@"
