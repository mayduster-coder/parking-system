#!/bin/bash
# =============================================================
# start_server.sh - build and start the parking TCP server + DB
#                   (parts 1 and 2 of the design document)
#
# Usage:  ./scripts/start_server.sh [config_file]
#         default config: server/server.conf
# =============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER_DIR="$SCRIPT_DIR/../server"
CONFIG="${1:-server.conf}"

cd "$SERVER_DIR" || { echo "[start] server directory not found"; exit 1; }

# 1. Check that the SQLite development library is installed
if ! ls /usr/include/sqlite3.h >/dev/null 2>&1; then
    echo "[start] libsqlite3-dev is missing. Install it with:"
    echo "        sudo apt install libsqlite3-dev"
    exit 1
fi

# 2. Stop an old instance that is still running (avoids 'Bind failed')
if pgrep -x server >/dev/null; then
    echo "[start] Stopping old server instance..."
    pkill -x server
    sleep 1
fi

# 3. Build
echo "[start] Building..."
make || { echo "[start] Build failed"; exit 1; }

# 4. Check the config file
if [ ! -f "$CONFIG" ]; then
    echo "[start] Warning: $CONFIG not found - the server will use defaults"
fi

# 5. Run (foreground, so the price menu works)
echo "[start] Starting server with config: $CONFIG"
exec ./server "$CONFIG"
