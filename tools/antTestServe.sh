#!/usr/bin/env bash
# antTestServe.sh — set up the venv and run the ESP32AntTest host server.
#
# Usage:  tools/antTestServe.sh [--background]
#
# Anchors to the repo root (parent of this script's directory) so it can be
# launched from anywhere. Creates/refreshes .venv, installs deps if needed,
# starts tools/server.py, and opens a browser. Foreground by default; pass
# --background to detach and open a browser after a short settle.
set -euo pipefail

# --- locate repo root ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# --- args ---
BACKGROUND=0
for a in "$@"; do
    case "$a" in
        --background|-b) BACKGROUND=1 ;;
        *) echo "unknown arg: $a" >&2; exit 2 ;;
    esac
done

# --- venv ---
if [ ! -d ".venv" ]; then
    echo "Creating virtual environment (.venv)…"
    python3 -m venv .venv
fi
# shellcheck disable=SC1091
source .venv/bin/activate

# --- deps (only when requirements.txt is newer than the stamp) ---
STAMP=".venv/.deps-stamp"
if [ ! -f "$STAMP" ] || [ tools/requirements.txt -nt "$STAMP" ]; then
    echo "Installing requirements…"
    pip install -q -r tools/requirements.txt
    touch "$STAMP"
fi

# --- start server ---
PORT="${ANT_TEST_PORT:-8000}"
echo "Starting server on http://127.0.0.1:${PORT}/  (logs at logs/host/)"

if [ "$BACKGROUND" -eq 1 ]; then
    python tools/server.py --port "$PORT" >/tmp/anttest-server.log 2>&1 &
    SERVER_PID=$!
    echo "Server PID $SERVER_PID (log: /tmp/anttest-server.log)"
    # give it a beat to bind, then open a browser
    sleep 1.5
    URL="http://127.0.0.1:${PORT}/"
    if command -v xdg-open >/dev/null 2>&1; then
        (xdg-open "$URL" >/dev/null 2>&1 || true) &
    elif command -v open >/dev/null 2>&1; then
        (open "$URL" >/dev/null 2>&1 || true) &
    else
        echo "Open $URL in your browser"
    fi
    echo "Stop with: kill $SERVER_PID  (or Ctrl+C does nothing in background mode)"
    wait "$SERVER_PID"
else
    # Foreground: server prints its own URL; Ctrl+C stops cleanly.
    exec python tools/server.py --port "$PORT"
fi
