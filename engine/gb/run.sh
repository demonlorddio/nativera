#!/bin/bash
# NativeRa Game Boy - Batocera Native Port Wrapper (run.sh)
# Follows the Batocera / Ports native port contract.

# 1. Inherit ONLY an allowlisted set of variables from PID 1.
#    Never import PID 1's full environment: it may carry credentials
#    (API keys, tokens) and run.sh output is tee'd into the log (step 3).
if [ -r /proc/1/environ ]; then
    while IFS= read -r envline; do
        case "$envline" in
            PATH=*|HOME=*|TERM=*|LANG=*|LC_*=*|DISPLAY=*|XDG_*)
                export "$envline"
                ;;
        esac
    done < <(tr '\0' '\n' < /proc/1/environ 2>/dev/null)
fi

# 2. Set display and isolated home
export DISPLAY=:0.0
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export HOME="${SCRIPT_DIR}/.home"
mkdir -p "$HOME"

# 3. Setup logging
LOG_DIR="/userdata/system/logs"
if [ ! -d "$LOG_DIR" ]; then
    LOG_DIR="${SCRIPT_DIR}/logs"
fi
mkdir -p "$LOG_DIR"
LOG_FILE="${LOG_DIR}/nativera-gb.log"

exec &> >(tee -a "$LOG_FILE")
echo "$(date): Launching NativeRa Game Boy Core..."

# 4. Locate binary and ROM
BINARY="${SCRIPT_DIR}/gb_emulator"
ROM="${1:-${SCRIPT_DIR}/roms/default.gb}"

if [ ! -x "$BINARY" ]; then
    echo "Error: Native binary not found or not executable at $BINARY"
    exit 1
fi

if [ ! -f "$ROM" ]; then
    echo "Error: ROM file not found at $ROM"
    echo "Usage: ./run.sh <rom.gb>"
    exit 1
fi

echo "Running binary: $BINARY with ROM: $ROM"
exec "$BINARY" "$ROM"
